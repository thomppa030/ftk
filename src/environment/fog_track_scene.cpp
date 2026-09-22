// The half of fog_track that looks at a scene's materials, which means the
// pipeline cache and so the renderer. Kept apart from the parse and fold so
// those build into the test library without Vulkan behind them.

#include "environment/fog_track.hpp"

#include "environment/environment_state.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/resources/material.hpp"
#include "scene/light_node.hpp"
#include "scene/scene.hpp"

#include <algorithm>

namespace fjell {

namespace fog_track {

namespace {

// The uniforms a fog node draws with: its material's shader, or the engine
// default for its kind when it names none.
const ShaderMetadata* fog_uniforms_of(const Material* material, std::string_view default_shader,
                                      PipelineCache* cache) {
    if (cache == nullptr) return nullptr;
    const uint32_t pid = material != nullptr
        ? material->pipeline_id()
        : cache->register_shader(std::string(default_shader));
    if (pid >= cache->shader_count()) return nullptr;
    const auto& meta = cache->metadata(pid);
    return meta.shader_type == ShaderType::Fog ? &meta : nullptr;
}

bool declares(const ShaderMetadata& meta, std::string_view uniform) {
    return std::any_of(meta.uniforms.begin(), meta.uniforms.end(), [&](const FjslUniform& u) {
        return u.type != UniformType::Sampler2D && u.name == uniform;
    });
}

void add_presets(const ShaderMetadata& meta, std::string_view group, std::vector<Preset>& out) {
    for (const auto& u : meta.uniforms) {
        if (u.type == UniformType::Sampler2D) continue;
        const std::string name = name_for(group, u.name);
        if (std::any_of(out.begin(), out.end(), [&](const Preset& p) { return p.name == name; })) {
            continue;
        }
        Preset preset;
        preset.name = name;
        preset.color = u.hint == UniformHint::Color;
        // Density scales what the node authored, so its range is a
        // multiplier's; anything else replaces the value and takes the
        // slider the shader declared for it.
        if (u.name == DENSITY_UNIFORM) {
            preset.range_min = 0.0f;
            preset.range_max = 4.0f;
        } else if (u.range.max > u.range.min) {
            preset.range_min = u.range.min;
            preset.range_max = u.range.max;
        }
        out.push_back(std::move(preset));
    }
}

} // namespace

std::vector<Preset> presets(const Scene& scene, PipelineCache* cache) {
    std::vector<Preset> out;
    for (const auto& obj : scene.objects()) {
        if (const auto* fog = dynamic_cast<const VolumetricFogNode*>(obj.get())) {
            if (const auto* meta = fog_uniforms_of(fog->material, DEFAULT_FOG_SHADER, cache)) {
                add_presets(*meta, {}, out);
            }
        } else if (const auto* volume = dynamic_cast<const FogVolumeNode*>(obj.get())) {
            if (volume->fog_group.empty()) continue;
            if (const auto* meta = fog_uniforms_of(volume->material, DEFAULT_FOG_VOLUME_SHADER, cache)) {
                add_presets(*meta, volume->fog_group, out);
            }
        }
    }
    return out;
}

std::string problem(std::string_view track_name, const Scene& scene, PipelineCache* cache) {
    const auto target = parse(track_name);
    if (!target) return {};

    if (target->group.empty()) {
        for (const auto& obj : scene.objects()) {
            if (const auto* fog = dynamic_cast<const VolumetricFogNode*>(obj.get())) {
                const auto* meta = fog_uniforms_of(fog->material, DEFAULT_FOG_SHADER, cache);
                if (meta == nullptr || declares(*meta, target->uniform)) return {};
                return "the fog node's material declares no uniform '" + target->uniform + "'";
            }
        }
        return "the scene has no Volumetric Fog node";
    }

    bool any_in_group = false;
    for (const auto& obj : scene.objects()) {
        const auto* volume = dynamic_cast<const FogVolumeNode*>(obj.get());
        if (volume == nullptr || volume->fog_group != target->group) continue;
        any_in_group = true;
        const auto* meta = fog_uniforms_of(volume->material, DEFAULT_FOG_VOLUME_SHADER, cache);
        if (meta == nullptr || declares(*meta, target->uniform)) return {};
    }
    if (!any_in_group) return "no fog volume in the scene is grouped '" + target->group + "'";
    return "no fog volume grouped '" + target->group + "' declares a uniform '"
         + target->uniform + "'";
}

} // namespace fog_track

} // namespace fjell
