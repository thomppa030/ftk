// The half of material_track that looks at a scene's materials, which means
// the pipeline cache and so the renderer. Kept apart from the parse and fold
// so those build into the test library without Vulkan behind them.

#include "environment/material_track.hpp"

#include "environment/environment_state.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/resources/material.hpp"
#include "scene/light_node.hpp"
#include "scene/scene.hpp"

#include <algorithm>

namespace fjell {

namespace material_track {

namespace {

// The uniforms a node draws with: its material's shader, or the engine
// default for its kind when it names none. Null when that is not a shader of
// the kind the node needs.
const ShaderMetadata* uniforms_of(const Material* material, std::string_view default_shader,
                                  ShaderType type, PipelineCache* cache) {
    if (cache == nullptr) return nullptr;
    const uint32_t pid = material != nullptr
        ? material->pipeline_id()
        : cache->register_shader(std::string(default_shader));
    if (pid >= cache->shader_count()) return nullptr;
    const auto& meta = cache->metadata(pid);
    return meta.shader_type == type ? &meta : nullptr;
}

bool declares(const ShaderMetadata& meta, std::string_view uniform) {
    return std::any_of(meta.uniforms.begin(), meta.uniforms.end(), [&](const FjslUniform& u) {
        return u.type != UniformType::Sampler2D && u.name == uniform;
    });
}

void add_presets(const ShaderMetadata& meta, Kind kind, std::string_view group,
                 std::vector<Preset>& out) {
    for (const auto& u : meta.uniforms) {
        if (u.type == UniformType::Sampler2D) continue;
        const std::string name = name_for(kind, group, u.name);
        if (std::any_of(out.begin(), out.end(), [&](const Preset& p) { return p.name == name; })) {
            continue;
        }
        Preset preset;
        preset.name = name;
        preset.heading = heading({kind, std::string(group), u.name});
        preset.color = u.hint == UniformHint::Color;
        // The fog's density scales what the node authored, so its range is a
        // multiplier's; anything else replaces the value and takes the
        // slider the shader declared for it, or its names' span.
        if (kind == Kind::Fog && u.name == FOG_DENSITY_UNIFORM) {
            preset.mode = TrackMode::Multiply;
            preset.range_min = 0.0f;
            preset.range_max = 4.0f;
        } else if (u.hint == UniformHint::Enum && !u.enum_options.empty()) {
            preset.range_min = 0.0f;
            preset.range_max = static_cast<float>(u.enum_options.size() - 1);
        } else if (u.range.max > u.range.min) {
            preset.range_min = u.range.min;
            preset.range_max = u.range.max;
        }
        out.push_back(std::move(preset));
    }
}

// Every node a target could reach, with the uniforms its material declares:
// the haze or a fog group's volumes, the infinite post volumes or a post
// group's. `each` returns true to stop.
template <typename Each>
void for_each_reached(const Scene& scene, const Target& target, PipelineCache* cache, Each&& each) {
    for (const auto& obj : scene.objects()) {
        if (target.kind == Kind::Fog) {
            if (target.group.empty()) {
                if (const auto* fog = dynamic_cast<const VolumetricFogNode*>(obj.get())) {
                    if (each(uniforms_of(fog->material, DEFAULT_FOG_SHADER, ShaderType::Fog, cache))) {
                        return;
                    }
                }
            } else if (const auto* volume = dynamic_cast<const FogVolumeNode*>(obj.get());
                       volume != nullptr && volume->fog_group == target.group) {
                if (each(uniforms_of(volume->material, DEFAULT_FOG_VOLUME_SHADER, ShaderType::Fog,
                                     cache))) {
                    return;
                }
            }
        } else if (const auto* post = dynamic_cast<const PostProcessVolumeNode*>(obj.get())) {
            const bool reached = target.group.empty() ? post->infinite_bounds
                                                      : post->post_group == target.group;
            if (reached && each(uniforms_of(post->material, DEFAULT_POST_SHADER, ShaderType::Post,
                                            cache))) {
                return;
            }
        }
    }
}

} // namespace

std::vector<Preset> presets(const Scene& scene, PipelineCache* cache) {
    std::vector<Preset> out;
    for (const auto& obj : scene.objects()) {
        if (const auto* fog = dynamic_cast<const VolumetricFogNode*>(obj.get())) {
            if (const auto* meta = uniforms_of(fog->material, DEFAULT_FOG_SHADER, ShaderType::Fog, cache)) {
                add_presets(*meta, Kind::Fog, {}, out);
            }
        } else if (const auto* volume = dynamic_cast<const FogVolumeNode*>(obj.get())) {
            if (volume->fog_group.empty()) continue;
            if (const auto* meta = uniforms_of(volume->material, DEFAULT_FOG_VOLUME_SHADER,
                                               ShaderType::Fog, cache)) {
                add_presets(*meta, Kind::Fog, volume->fog_group, out);
            }
        } else if (const auto* post = dynamic_cast<const PostProcessVolumeNode*>(obj.get())) {
            const auto* meta = uniforms_of(post->material, DEFAULT_POST_SHADER, ShaderType::Post, cache);
            if (meta == nullptr) continue;
            // An infinite volume answers the ungrouped tracks; any volume
            // answers its group's.
            if (post->infinite_bounds) add_presets(*meta, Kind::Post, {}, out);
            if (!post->post_group.empty()) add_presets(*meta, Kind::Post, post->post_group, out);
        }
    }
    return out;
}

std::string problem(std::string_view track_name, const Scene& scene, PipelineCache* cache) {
    const auto target = parse(track_name);
    if (!target) return {};

    bool any = false;
    bool declared = false;
    for_each_reached(scene, *target, cache, [&](const ShaderMetadata* meta) {
        any = true;
        // A node whose shader could not be read says nothing either way.
        declared = meta == nullptr || declares(*meta, target->uniform);
        return declared;
    });
    if (declared) return {};

    const std::string uniform = "'" + target->uniform + "'";
    if (target->kind == Kind::Fog) {
        if (target->group.empty()) {
            return any ? "the fog node's material declares no uniform " + uniform
                       : "the scene has no Volumetric Fog node";
        }
        return any ? "no fog volume grouped '" + target->group + "' declares a uniform " + uniform
                   : "no fog volume in the scene is grouped '" + target->group + "'";
    }
    if (target->group.empty()) {
        return any ? "no infinite post process volume's material declares a uniform " + uniform
                   : "the scene has no post process volume with infinite bounds";
    }
    return any ? "no post process volume grouped '" + target->group + "' declares a uniform " + uniform
               : "no post process volume in the scene is grouped '" + target->group + "'";
}

} // namespace material_track

} // namespace fjell
