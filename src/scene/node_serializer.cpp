#include "scene/node_serializer.hpp"
#include "scene/node.hpp"
#include "scene/component_registry.hpp"
#include "scene/camera_node.hpp"
#include "scene/light_node.hpp"
#include "animation/animation_clip.hpp"
#include "animation/animator_component.hpp"
#include "renderer/components/mesh_component.hpp"
#include "renderer/systems/vfx_asset.hpp"
#include "renderer/systems/vfx_emitter.hpp"
#include "renderer/components/skeletal_mesh_component.hpp"
#include "renderer/resources/material.hpp"
#include "renderer/resources/model.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/resources/skeletal_model.hpp"
#include "renderer/resources/texture.hpp"
#include "scripting/fjell/component.hpp"
#include "core/base_path.hpp"
#include "ftk/math/color_space.hpp"
#include "core/project.hpp"
#include "environment/environment_state.hpp"
#include "ftk/base/log.hpp"
#include "environment/weather_asset.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace fjell {

nlohmann::json serialize_node(const Node& node, const Project* project) {
    nlohmann::json j;
    j["id"] = node.id;
    j["parent_id"] = node.parent_id;
    j["name"] = node.name;
    // The model a mesh node draws. Primitives are a built-in name rather than
    // an asset, so asset_reference leaves them alone.
    j["source"] = base_path::asset_reference(node.source, project);
    if (!node.preset_source.empty()) {
        j["preset_source"] = node.preset_source;
    }

    if (!node.is_spatial()) {
        j["node_type"] = "folder";
    }

    // Components (via registry)
    auto& registry = ComponentRegistry::instance();
    node.components.for_each([&](ComponentTypeId id, const void* ptr) {
        auto* meta = registry.find(id);
        if (!meta) return;
        nlohmann::json comp_json;
        meta->serialize(ptr, node, comp_json, project);
        if (!comp_json.empty()) {
            j[meta->json_key] = std::move(comp_json);
        }
    });

    // EmitterComponent serializes VFX asset path via _vfx_path sentinel;
    // whatever else it wrote (looping) stays under its own key
    if (j.contains("emitter") && j["emitter"].contains("_vfx_path")) {
        j["vfx"] = j["emitter"]["_vfx_path"];
        j["emitter"].erase("_vfx_path");
        if (j["emitter"].empty()) j.erase("emitter");
    }

    // Transform (spatial nodes only)
    if (auto* spatial = node.as_spatial()) {
        j["position"] = {spatial->transform.position.x, spatial->transform.position.y, spatial->transform.position.z};
        {
            auto euler = spatial->transform.euler_degrees();
            j["rotation"] = {euler.x, euler.y, euler.z};
        }
        j["scale"] = {spatial->transform.scale.x, spatial->transform.scale.y, spatial->transform.scale.z};
    }

    // Scripts
    if (!node.scripts.empty()) {
        j["components"] = nlohmann::json::array();
        for (const auto& comp : node.scripts) {
            nlohmann::json jcomp;
            jcomp["type"] = comp.type_name;
            jcomp["data"] = comp.serialized;
            j["components"].push_back(jcomp);
        }
    }

    // Typed node data (node_type + type-specific properties). The fog
    // materials are asset references, written as ids where the project
    // tracks the asset like every other reference in the file.
    j.update(serialize_node_type(node));
    for (const char* key : {"fog", "fog_volume"}) {
        if (j.contains(key) && j[key].contains("material")) {
            j[key]["material"] = base_path::asset_reference(j[key]["material"].get<std::string>(), project);
        }
    }

    return j;
}

// ── Deserialize ──────────────────────────────────────────────────────────

// Helper: parse transform from JSON
static Transform parse_transform(const nlohmann::json& j) {
    Transform t;
    if (j.contains("position")) {
        auto& p = j["position"];
        t.position = {p[0].get<float>(), p[1].get<float>(), p[2].get<float>()};
    }
    if (j.contains("rotation")) {
        auto& r = j["rotation"];
        t.set_euler_degrees({r[0].get<float>(), r[1].get<float>(), r[2].get<float>()});
    }
    if (j.contains("scale")) {
        auto& s = j["scale"];
        t.scale = {s[0].get<float>(), s[1].get<float>(), s[2].get<float>()};
    }
    return t;
}

// Helper: load common components onto a node
static void load_components(Node& node, const nlohmann::json& j,
                            ResourceManager& resources,
                            const Project* project) {
    // Components (via registry)
    auto& registry = ComponentRegistry::instance();
    registry.for_each([&](ComponentTypeId /*id*/, const ComponentMeta& meta) {
        if (j.contains(meta.json_key)) {
            meta.deserialize(node, j[meta.json_key], resources, project);
        }
    });

    // VFX asset loading (uses "vfx" key, not handled by emitter meta's "emitter" key)
    if (j.contains("vfx")) {
        std::string vfx_path = j["vfx"].get<std::string>();
        auto resolved = base_path::resolve_asset(vfx_path, project);
        try {
            auto effect = load_vfx_effect(resolved);
            resolve_vfx_textures(effect, resources, project);

            auto& ec = node.components.get<EmitterComponent>()
                ? *node.components.get<EmitterComponent>()
                : node.components.add<EmitterComponent>();
            ec.effect = std::make_shared<VfxEffect>(std::move(effect));
            ec.vfx_path = vfx_path;
        } catch (const std::exception& e) {
            FTK_CORE_ERROR("Failed to load VFX: {}", e.what());
        }
    }

    if (j.contains("components")) {
        for (const auto& jcomp : j["components"]) {
            ScriptEntry comp;
            comp.type_name = jcomp.value("type", "");
            // A node holds one script of a type; a file that lists one twice
            // was written by hand or by an engine that allowed it.
            if (std::ranges::find(node.scripts, comp.type_name, &ScriptEntry::type_name) != node.scripts.end()) {
                FTK_CORE_WARN("'{}' lists {} more than once; the first is kept", node.name,
                                comp.type_name);
                continue;
            }
            if (jcomp.contains("data")) {
                comp.serialized = jcomp["data"];
            }
            node.scripts.push_back(std::move(comp));
        }
    }
}

std::unique_ptr<Node> deserialize_node(const nlohmann::json& j,
                                       ResourceManager& resources,
                                       const Project* project) {
    std::string name = j.value("name", "Object");
    std::string source = j.value("source", "");
    std::string preset_src = j.value("preset_source", "");
    uint32_t loaded_id = j.value("id", 0u);
    int32_t loaded_parent = j.value("parent_id", -1);
    Transform transform = parse_transform(j);

    // Apply preset_source to the returned node
    auto apply_preset = [&](std::unique_ptr<Node>& node) {
        if (node && !preset_src.empty()) node->preset_source = preset_src;
    };

    std::string node_type = j.value("node_type", "");

    // ── Typed nodes ──────────────────────────────────────────────────

    if (node_type == "folder") {
        auto folder = std::make_unique<Node>();
        folder->name = name;
        folder->source = source;
        folder->id = loaded_id;
        folder->parent_id = loaded_parent;
        load_components(*folder, j, resources, project);
        apply_preset(folder);
        return folder;
    }

    if (!node_type.empty()) {
        std::unique_ptr<Node3D> node;

        if (node_type == "camera") {
            node = std::make_unique<CameraNode>();
        } else if (node_type == "directional_light") {
            node = std::make_unique<DirectionalLightNode>();
        } else if (node_type == "point_light") {
            node = std::make_unique<PointLightNode>();
        } else if (node_type == "spot_light") {
            node = std::make_unique<SpotLightNode>();
        } else if (node_type == "volumetric_fog") {
            node = std::make_unique<VolumetricFogNode>();
        } else if (node_type == "fog_volume") {
            node = std::make_unique<FogVolumeNode>();
        } else if (node_type == "post_process_volume") {
            node = std::make_unique<PostProcessVolumeNode>();
        } else if (node_type == "world_environment") {
            node = std::make_unique<WorldEnvironmentNode>();
        } else if (node_type == "game_ui") {
            // Legacy: GameUiNode no longer supported, load as empty Node3D
            FTK_CORE_WARN("game_ui node type is deprecated — use UiWidgetComponent with .fjwidget");
            node = std::make_unique<Node3D>();
        }

        if (node) {
            apply_node_type(*node, j);
            node->name = name;
            node->source = source;
            node->transform = transform;
            node->id = loaded_id;
            node->parent_id = loaded_parent;
            load_components(*node, j, resources, project);
            if (!preset_src.empty()) node->preset_source = preset_src;
            return node;
        }
    }

    // ── Skeletal mesh ────────────────────────────────────────────────

    if (j.contains("skeletal_mesh") && !source.empty()) {
        const auto& jsm = j["skeletal_mesh"];

        auto skel_model_result = resources.load_skeletal_model(
            base_path::resolve_asset(source, project));
        if (!skel_model_result) {
            FTK_CORE_ERROR("deserialize_node: skeletal '{}' load failed: {}",
                             source, skel_model_result.error());
            return nullptr;
        }

        auto skel_model_handle = *skel_model_result;
        auto* skel_model = resources.get(skel_model_handle);
        if (!skel_model) {
            FTK_CORE_ERROR("deserialize_node: skeletal '{}' handle resolved null", source);
            return nullptr;
        }

        auto node = std::make_unique<Node3D>();
        node->name = name;
        node->source = source;
        node->transform = transform;
        node->id = loaded_id;
        node->parent_id = loaded_parent;

        // SkeletalMeshComponent — rendering only (model + material)
        auto& smc = node->components.add<SkeletalMeshComponent>();
        smc.skeletal_model_handle = skel_model_handle;
        smc.skeletal_model = skel_model;
        smc.material_name = jsm.value("material", "default");

        // Material
        auto mat_n = smc.material_name;
        if (base_path::references_asset(mat_n, ".fjmat")) {
            auto mat_result = resources.load_material_asset(
                base_path::resolve_asset(mat_n, project), project);
            if (mat_result) {
                smc.material_handle = *mat_result;
                smc.material = resources.get(smc.material_handle);
            }
        }
        if (!smc.material) {
            auto default_tex = resources.load_texture(
                base_path::resolve("engine_assets/textures/test.png")).value_or(TextureHandle{});
            smc.material_handle = resources.create_material("skel_default", default_tex);
            smc.material = resources.get(smc.material_handle);
        }

        // AnimatorComponent — skeleton comes from the SkeletalModel
        auto& anim = node->components.add<AnimatorComponent>();
        if (skel_model->skeleton()) {
            anim.init(skel_model->skeleton());
        }

        // Animation clips + animset from "animator" block
        if (j.contains("animator")) {
            const auto& ja = j["animator"];
            if (ja.contains("clips") && ja["clips"].is_array()) {
                for (const auto& jc : ja["clips"]) {
                    std::string clip_rel = jc.get<std::string>();
                    anim.clip_paths.push_back(clip_rel);
                    auto clip_result = AnimationClip::load(base_path::resolve_asset(clip_rel, project));
                    if (clip_result) {
                        auto clip = std::make_shared<AnimationClip>(std::move(*clip_result));
                        anim.clips.push_back(clip);
                        if (anim.clips.size() == 1) {
                            anim.play(clip.get());
                        }
                    } else {
                        FTK_CORE_ERROR("Failed to load clip {}: {}", clip_rel, clip_result.error());
                    }
                }
            }
            if (ja.contains("animset")) {
                anim.animset_path = ja["animset"].get<std::string>();
                auto resolved = base_path::resolve_asset(anim.animset_path, project);
                if (auto r = anim.load_state_machine(resolved); !r) {
                    FTK_CORE_ERROR("Failed to load animset {}: {}", anim.animset_path, r.error());
                }
                for (size_t i = 0; i < anim.clips.size(); ++i) {
                    if (i < anim.clip_paths.size())
                        anim.set_clip(anim.clip_paths[i], anim.clips[i].get());
                }
                std::string base_dir = project ? project->root().string() : "";
                anim.resolve_blend_spaces(base_dir);
            }
        }

        load_components(*node, j, resources, project);
        if (!preset_src.empty()) node->preset_source = preset_src;
        return node;
    }

    // ── Mesh nodes or empty Node3D ───────────────────────────────────

    std::string mat_name = "default";
    nlohmann::json color_json;
    if (j.contains("mesh")) {
        const auto& jmesh = j["mesh"];
        mat_name = jmesh.value("material", "default");
        if (jmesh.contains("color")) color_json = jmesh["color"];
    } else {
        mat_name = j.value("material", "default");
        if (j.contains("color")) color_json = j["color"];
    }

    bool has_mesh = !source.empty() || j.contains("mesh") || j.contains("material");

    if (has_mesh) {
        ModelHandle model_handle;
        if (source.starts_with("primitive:")) {
            std::string type = source.substr(10);
            auto result = resources.create_primitive(type);
            if (result) model_handle = *result;
        } else if (!source.empty()) {
            auto resolved = base_path::resolve_asset(source, project);
            if (resolved.empty()) {
                FTK_CORE_WARN("Object '{}': model reference '{}' does not resolve",
                                name, source);
            } else {
                auto result = resources.load_model(resolved);
                if (result) {
                    model_handle = *result;
                } else {
                    FTK_CORE_WARN("Object '{}': {}", name, result.error());
                }
            }
        }

        // Keep the object even with no model. Dropping it loses the transform,
        // the hierarchy under it and every other component, and saving the
        // scene afterwards makes that permanent — for what is usually a
        // renamed or temporarily missing asset.
        if (!model_handle.valid()) {
            FTK_CORE_WARN("Object '{}' has no usable model; keeping it without one", name);
        }

        auto default_tex = resources.load_texture(
            base_path::resolve("engine_assets/textures/test.png")).value_or(TextureHandle{});

        // A material reference is either a path or an asset id; both resolve
        // through resolve_asset, and neither is the "default" sentinel.
        bool has_material_ref = mat_name.ends_with(".fjmat") ||
                                base_path::is_asset_id(mat_name);

        MaterialHandle mat_handle;
        if (has_material_ref) {
            std::string mat_path = base_path::resolve_asset(mat_name, project);
            if (mat_path.empty()) {
                FTK_CORE_WARN("Object '{}': material reference '{}' does not resolve",
                                name, mat_name);
            } else {
                auto result = resources.load_material_asset(mat_path, project);
                if (result) {
                    mat_handle = *result;
                } else {
                    FTK_CORE_WARN("Object '{}': {}", name, result.error());
                }
            }
        } else if (!color_json.is_null()) {
            auto& c = color_json;
            PbrParams params{};
            float alpha = c.size() > 3 ? c[3].get<float>() : 1.0f;
            params.base_color_factor = {c[0].get<float>(), c[1].get<float>(),
                                        c[2].get<float>(), alpha};
            char color_key[64];
            std::snprintf(color_key, sizeof(color_key), "color_%.3f_%.3f_%.3f_%.3f",
                          params.base_color_factor.x, params.base_color_factor.y,
                          params.base_color_factor.z, params.base_color_factor.w);
            mat_handle = resources.create_material(color_key,
                resources.default_white_handle(), {}, {}, {}, params);
        } else {
            mat_handle = resources.create_material(mat_name, default_tex);
        }

        auto* model = resources.get(model_handle);
        if (!has_material_ref &&
            !source.starts_with("primitive:") && model && !model->textures().empty()) {
            mat_handle = resources.create_material_from_texture(
                source + "_mat",
                const_cast<Texture*>(model->textures()[0].get()));
        }

        auto node = std::make_unique<Node3D>();
        node->name = name;
        node->source = source;
        node->transform = transform;
        node->id = loaded_id;
        node->parent_id = loaded_parent;

        auto& mc = node->components.add<MeshComponent>();
        mc.model_handle = model_handle;
        mc.model = resources.get(model_handle);
        mc.material_handle = mat_handle;
        mc.material = resources.get(mat_handle);
        mc.material_name = mat_name;
        mc.slot_materials = resources.load_slot_materials(model_handle, project);

        load_components(*node, j, resources, project);
        if (!preset_src.empty()) node->preset_source = preset_src;
        return node;
    }

    // Empty Node3D
    auto node = std::make_unique<Node3D>();
    node->name = name;
    node->source = source;
    node->transform = transform;
    node->id = loaded_id;
    node->parent_id = loaded_parent;
    load_components(*node, j, resources, project);
    if (!preset_src.empty()) node->preset_source = preset_src;
    return node;
}

} // namespace fjell
