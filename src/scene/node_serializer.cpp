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
#include "core/math/color_space.hpp"
#include "core/project.hpp"
#include "core/log.hpp"

#include <nlohmann/json.hpp>

namespace fjell {

nlohmann::json color_to_json(glm::vec3 linear) {
    const glm::vec3 c = color_space::linear_to_srgb(linear);
    return {c.x, c.y, c.z};
}

glm::vec3 color_from_json(const nlohmann::json& srgb) {
    return color_space::srgb_to_linear(
        glm::vec3{srgb[0].get<float>(), srgb[1].get<float>(), srgb[2].get<float>()});
}

// ── Serialize ────────────────────────────────────────────────────────────

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

    // EmitterComponent serializes VFX asset path via _vfx_path sentinel
    if (j.contains("emitter") && j["emitter"].contains("_vfx_path")) {
        j["vfx"] = j["emitter"]["_vfx_path"];
        j.erase("emitter");
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

    // Typed node data (node_type + type-specific properties)
    if (auto* cam = dynamic_cast<const CameraNode*>(&node)) {
        j["node_type"] = "camera";
        nlohmann::json jc;
        jc["fov"] = cam->fov;
        jc["near"] = cam->near_plane;
        jc["far"] = cam->far_plane;
        if (cam->projection != CameraProjection::perspective) {
            jc["projection"] = static_cast<int>(cam->projection);
        }
        if (cam->ortho_size != 10.0f) {
            jc["ortho_size"] = cam->ortho_size;
        }
        j["camera"] = jc;
    } else if (auto* dl = dynamic_cast<const DirectionalLightNode*>(&node)) {
        j["node_type"] = "directional_light";
        j["light"] = {
            {"direction", {dl->direction.x, dl->direction.y, dl->direction.z}},
            {"color", color_to_json(dl->color)},
            {"intensity", dl->intensity},
            {"ambient_color", color_to_json(dl->ambient_color)},
            {"ambient_intensity", dl->ambient_intensity}
        };
    } else if (auto* pl = dynamic_cast<const PointLightNode*>(&node)) {
        j["node_type"] = "point_light";
        j["light"] = {
            {"color", color_to_json(pl->color)},
            {"intensity", pl->intensity},
            {"radius", pl->radius},
            {"vol_scatter_intensity", pl->vol_scatter_intensity},
            {"cast_shadows", pl->cast_shadows},
            {"shadow_bias", pl->shadow_bias}
        };
    } else if (auto* sl = dynamic_cast<const SpotLightNode*>(&node)) {
        j["node_type"] = "spot_light";
        j["light"] = {
            {"direction", {sl->direction.x, sl->direction.y, sl->direction.z}},
            {"color", color_to_json(sl->color)},
            {"intensity", sl->intensity},
            {"radius", sl->radius},
            {"inner_angle", sl->inner_angle},
            {"outer_angle", sl->outer_angle},
            {"vol_scatter_intensity", sl->vol_scatter_intensity},
            {"cast_shadows", sl->cast_shadows},
            {"shadow_bias", sl->shadow_bias}
        };
    } else if (auto* vf = dynamic_cast<const VolumetricFogNode*>(&node)) {
        j["node_type"] = "volumetric_fog";
        j["fog"] = {
            {"density", vf->density},
            {"height_falloff", vf->height_falloff},
            {"scattering", vf->scattering},
            {"anisotropy", vf->anisotropy},
            {"max_distance", vf->max_distance},
            {"temporal_blend", vf->temporal_blend},
            {"color", color_to_json(vf->fog_color)},
            {"shafts_enabled", vf->shafts_enabled},
            {"shaft_intensity", vf->shaft_intensity},
            {"shaft_density_mult", vf->shaft_density_mult},
            {"shaft_max_reach", vf->shaft_max_reach},
            {"shaft_step_count", vf->shaft_step_count},
            {"shaft_anisotropy", vf->shaft_anisotropy},
            {"shaft_directionality", vf->shaft_directionality},
            {"shaft_threshold", vf->shaft_threshold},
            {"shaft_halo_inner", vf->shaft_halo_inner},
            {"shaft_halo_outer", vf->shaft_halo_outer}
        };
    } else if (auto* fv = dynamic_cast<const FogVolumeNode*>(&node)) {
        j["node_type"] = "fog_volume";
        j["fog_volume"] = {
            {"shape", static_cast<int>(fv->shape)},
            {"density", fv->density},
            {"color", color_to_json(fv->fog_color)},
            {"scattering_mult", fv->scattering_mult},
            {"edge_falloff", fv->edge_falloff},
            {"noise_contrast", fv->noise_contrast},
            {"noise_scale", fv->noise_scale},
            {"noise_speed", {fv->noise_speed.x, fv->noise_speed.y, fv->noise_speed.z}},
            {"wind_influence", fv->wind_influence},
            {"noise_octaves", fv->noise_octaves}
        };
    } else if (auto* env = dynamic_cast<const WorldEnvironmentNode*>(&node)) {
        j["node_type"] = "world_environment";
        j["sky"] = {
            {"sky_top_color", color_to_json(env->sky_top_color)},
            {"sky_bottom_color", color_to_json(env->sky_bottom_color)},
            {"ibl_intensity", env->ibl_intensity},
            {"contact_shadows_enabled", env->contact_shadows_enabled},
            {"contact_shadow_distance", env->contact_shadow_distance},
            {"contact_shadow_thickness", env->contact_shadow_thickness},
            {"contact_shadow_steps", env->contact_shadow_steps},
            {"atmosphere_enabled", env->atmosphere_enabled},
            {"atmosphere_rayleigh_scale", env->atmosphere_rayleigh_scale},
            {"atmosphere_mie_scale", env->atmosphere_mie_scale},
            {"atmosphere_mie_g", env->atmosphere_mie_g},
            {"atmosphere_mie_coeff", env->atmosphere_mie_coeff},
            {"atmosphere_sun_intensity", env->atmosphere_sun_intensity},
            {"atmosphere_hdri_floor", env->atmosphere_hdri_floor}
        };
        if (!env->hdri_path.empty()) {
            j["sky"]["hdri_path"] = env->hdri_path;
        }
        if (!env->sky_shader.empty()) {
            j["sky"]["sky_shader"] = env->sky_shader;
        }
        if (!env->day_profile.empty()) {
            j["day_profile"] = env->day_profile;
        }
        if (!env->weather.empty()) {
            j["weather"] = env->weather;
        }
        if (!env->climate.empty()) {
            j["climate"] = env->climate;
        }
        if (!env->sky_uniforms.empty()) {
            auto& ju = j["sky_uniforms"] = nlohmann::json::object();
            for (const auto& u : env->sky_uniforms) {
                auto values = nlohmann::json::array();
                for (uint32_t i = 0; i < u.count && i < u.values.size(); ++i) values.push_back(u.values[i]);
                ju[u.name] = std::move(values);
            }
        }
        j["time_of_day"] = {
            {"enabled", env->time_of_day_enabled},
            {"hour", env->time_of_day},
            {"day_length", env->day_length},
            {"advance_in_editor", env->advance_in_editor},
            {"sun_path_tilt", env->sun_path_tilt},
            {"north_yaw", env->north_yaw},
            {"noon_hour", env->noon_hour},
            {"moon_offset_hours", env->moon_offset_hours},
            {"moon_phase", env->moon_phase},
            {"moon_color", color_to_json(env->moon_color)},
            {"moon_intensity", env->moon_intensity}
        };
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
            FJELL_CORE_ERROR("Failed to load VFX: {}", e.what());
        }
    }

    if (j.contains("components")) {
        for (const auto& jcomp : j["components"]) {
            ScriptEntry comp;
            comp.type_name = jcomp.value("type", "");
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
            auto cam = std::make_unique<CameraNode>();
            if (j.contains("camera")) {
                auto& jc = j["camera"];
                cam->fov = jc.value("fov", 45.0f);
                cam->near_plane = jc.value("near", 0.1f);
                cam->far_plane = jc.value("far", 1000.0f);
                cam->projection = static_cast<CameraProjection>(jc.value("projection", 0));
                cam->ortho_size = jc.value("ortho_size", 10.0f);
            }
            node = std::move(cam);
        } else if (node_type == "directional_light") {
            auto dl = std::make_unique<DirectionalLightNode>();
            if (j.contains("light")) {
                auto& jl = j["light"];
                if (jl.contains("direction")) {
                    auto& d = jl["direction"];
                    dl->direction = {d[0].get<float>(), d[1].get<float>(), d[2].get<float>()};
                }
                if (jl.contains("color")) {
                    auto& c = jl["color"];
                    dl->color = color_from_json(c);
                }
                dl->intensity = jl.value("intensity", 1.5f);
                if (jl.contains("ambient_color")) {
                    auto& c = jl["ambient_color"];
                    dl->ambient_color = color_from_json(c);
                }
                dl->ambient_intensity = jl.value("ambient_intensity", 1.0f);
            }
            node = std::move(dl);
        } else if (node_type == "point_light") {
            auto pl = std::make_unique<PointLightNode>();
            if (j.contains("light")) {
                auto& jl = j["light"];
                if (jl.contains("color")) {
                    auto& c = jl["color"];
                    pl->color = color_from_json(c);
                }
                pl->intensity = jl.value("intensity", 1.0f);
                pl->radius = jl.value("radius", 10.0f);
                pl->vol_scatter_intensity = jl.value("vol_scatter_intensity", 1.0f);
                pl->cast_shadows = jl.value("cast_shadows", false);
                pl->shadow_bias = jl.value("shadow_bias", 0.005f);
            }
            node = std::move(pl);
        } else if (node_type == "spot_light") {
            auto sl = std::make_unique<SpotLightNode>();
            if (j.contains("light")) {
                auto& jl = j["light"];
                if (jl.contains("direction")) {
                    auto& d = jl["direction"];
                    sl->direction = {d[0].get<float>(), d[1].get<float>(), d[2].get<float>()};
                }
                if (jl.contains("color")) {
                    auto& c = jl["color"];
                    sl->color = color_from_json(c);
                }
                sl->intensity = jl.value("intensity", 1.0f);
                sl->radius = jl.value("radius", 10.0f);
                sl->inner_angle = jl.value("inner_angle", 20.0f);
                sl->outer_angle = jl.value("outer_angle", 30.0f);
                sl->vol_scatter_intensity = jl.value("vol_scatter_intensity", 1.0f);
                sl->cast_shadows = jl.value("cast_shadows", false);
                sl->shadow_bias = jl.value("shadow_bias", 0.002f);
            }
            node = std::move(sl);
        } else if (node_type == "volumetric_fog") {
            auto vf = std::make_unique<VolumetricFogNode>();
            if (j.contains("fog")) {
                auto& jf = j["fog"];
                vf->density = jf.value("density", 0.003f);
                vf->height_falloff = jf.value("height_falloff", 0.5f);
                vf->scattering = jf.value("scattering", 0.5f);
                vf->anisotropy = jf.value("anisotropy", 0.7f);
                vf->max_distance = jf.value("max_distance", 130.0f);
                vf->temporal_blend = jf.value("temporal_blend", 0.9f);
                if (jf.contains("color")) {
                    auto& c = jf["color"];
                    vf->fog_color = color_from_json(c);
                }
                vf->shafts_enabled = jf.value("shafts_enabled", true);
                vf->shaft_intensity = jf.value("shaft_intensity", 1.0f);
                vf->shaft_density_mult = jf.value("shaft_density_mult", 1.0f);
                vf->shaft_max_reach = jf.value("shaft_max_reach", 200.0f);
                vf->shaft_step_count = jf.value("shaft_step_count", 32);
                vf->shaft_anisotropy = jf.value("shaft_anisotropy", 0.75f);
                vf->shaft_directionality = jf.value("shaft_directionality", 1.0f);
                vf->shaft_threshold = jf.value("shaft_threshold", 0.0f);
                vf->shaft_halo_inner = jf.value("shaft_halo_inner", 0.95f);
                vf->shaft_halo_outer = jf.value("shaft_halo_outer", 0.6f);
            }
            node = std::move(vf);
        } else if (node_type == "fog_volume") {
            auto fv = std::make_unique<FogVolumeNode>();
            if (j.contains("fog_volume")) {
                auto& jv = j["fog_volume"];
                fv->shape = static_cast<ColliderShape>(jv.value("shape", 0));
                fv->density = jv.value("density", 0.05f);
                if (jv.contains("color")) {
                    fv->fog_color = color_from_json(jv["color"]);
                }
                fv->scattering_mult = jv.value("scattering_mult", 1.0f);
                fv->edge_falloff = jv.value("edge_falloff", 0.2f);
                fv->noise_contrast = jv.value("noise_contrast", 0.0f);
                fv->noise_scale = jv.value("noise_scale", 8.0f);
                if (jv.contains("noise_speed")) {
                    auto& s = jv["noise_speed"];
                    fv->noise_speed = {s[0].get<float>(), s[1].get<float>(), s[2].get<float>()};
                }
                fv->wind_influence = jv.value("wind_influence", 0.0f);
                fv->noise_octaves = jv.value("noise_octaves", 2);
            }
            node = std::move(fv);
        } else if (node_type == "world_environment") {
            auto env = std::make_unique<WorldEnvironmentNode>();
            if (j.contains("sky")) {
                auto& js = j["sky"];
                if (js.contains("sky_top_color")) {
                    auto& c = js["sky_top_color"];
                    env->sky_top_color = color_from_json(c);
                }
                if (js.contains("sky_bottom_color")) {
                    auto& c = js["sky_bottom_color"];
                    env->sky_bottom_color = color_from_json(c);
                }
                env->ibl_intensity = js.value("ibl_intensity", 1.0f);
                env->contact_shadows_enabled = js.value("contact_shadows_enabled", true);
                env->contact_shadow_distance = js.value("contact_shadow_distance", 0.3f);
                env->contact_shadow_thickness = js.value("contact_shadow_thickness", 0.05f);
                env->contact_shadow_steps = js.value("contact_shadow_steps", 12);
                env->atmosphere_enabled = js.value("atmosphere_enabled", false);
                env->atmosphere_rayleigh_scale = js.value("atmosphere_rayleigh_scale", 8500.0f);
                env->atmosphere_mie_scale = js.value("atmosphere_mie_scale", 1200.0f);
                env->atmosphere_mie_g = js.value("atmosphere_mie_g", 0.76f);
                env->atmosphere_mie_coeff = js.value("atmosphere_mie_coeff", 21e-6f);
                env->atmosphere_sun_intensity = js.value("atmosphere_sun_intensity", 22.0f);
                env->atmosphere_hdri_floor = js.value("atmosphere_hdri_floor", 0.1f);
                if (js.contains("hdri_path")) {
                    env->hdri_path = js["hdri_path"].get<std::string>();
                }
                if (js.contains("sky_shader")) {
                    env->sky_shader = js["sky_shader"].get<std::string>();
                }
            }
            env->day_profile = j.value("day_profile", std::string{});
            env->weather = j.value("weather", std::string{});
            env->climate = j.value("climate", std::string{});
            if (j.contains("sky_uniforms") && j["sky_uniforms"].is_object()) {
                for (const auto& [uniform_name, values] : j["sky_uniforms"].items()) {
                    if (!values.is_array() || values.empty()) continue;
                    WorldEnvironmentNode::SkyUniform u;
                    u.name = uniform_name;
                    u.count = static_cast<uint32_t>(std::min<size_t>(values.size(), u.values.size()));
                    for (uint32_t i = 0; i < u.count; ++i) u.values[i] = values[i].get<float>();
                    env->sky_uniforms.push_back(std::move(u));
                }
            }
            if (j.contains("time_of_day")) {
                auto& jt = j["time_of_day"];
                env->time_of_day_enabled = jt.value("enabled", false);
                env->time_of_day = jt.value("hour", 12.0f);
                env->day_length = jt.value("day_length", 1200.0f);
                env->advance_in_editor = jt.value("advance_in_editor", false);
                env->sun_path_tilt = jt.value("sun_path_tilt", 40.0f);
                env->north_yaw = jt.value("north_yaw", 0.0f);
                env->noon_hour = jt.value("noon_hour", 12.0f);
                env->moon_offset_hours = jt.value("moon_offset_hours", 12.0f);
                env->moon_phase = jt.value("moon_phase", 0.5f);
                if (jt.contains("moon_color")) env->moon_color = color_from_json(jt["moon_color"]);
                env->moon_intensity = jt.value("moon_intensity", 0.08f);
            }
            node = std::move(env);
        } else if (node_type == "game_ui") {
            // Legacy: GameUiNode no longer supported, load as empty Node3D
            FJELL_CORE_WARN("game_ui node type is deprecated — use UiWidgetComponent with .fjwidget");
            node = std::make_unique<Node3D>();
        }

        if (node) {
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
            FJELL_CORE_ERROR("deserialize_node: skeletal '{}' load failed: {}",
                             source, skel_model_result.error());
            return nullptr;
        }

        auto skel_model_handle = *skel_model_result;
        auto* skel_model = resources.get(skel_model_handle);
        if (!skel_model) {
            FJELL_CORE_ERROR("deserialize_node: skeletal '{}' handle resolved null", source);
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
                        FJELL_CORE_ERROR("Failed to load clip {}: {}", clip_rel, clip_result.error());
                    }
                }
            }
            if (ja.contains("animset")) {
                anim.animset_path = ja["animset"].get<std::string>();
                auto resolved = base_path::resolve_asset(anim.animset_path, project);
                if (auto r = anim.load_state_machine(resolved); !r) {
                    FJELL_CORE_ERROR("Failed to load animset {}: {}", anim.animset_path, r.error());
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
                FJELL_CORE_WARN("Object '{}': model reference '{}' does not resolve",
                                name, source);
            } else {
                auto result = resources.load_model(resolved);
                if (result) {
                    model_handle = *result;
                } else {
                    FJELL_CORE_WARN("Object '{}': {}", name, result.error());
                }
            }
        }

        // Keep the object even with no model. Dropping it loses the transform,
        // the hierarchy under it and every other component, and saving the
        // scene afterwards makes that permanent — for what is usually a
        // renamed or temporarily missing asset.
        if (!model_handle.valid()) {
            FJELL_CORE_WARN("Object '{}' has no usable model; keeping it without one", name);
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
                FJELL_CORE_WARN("Object '{}': material reference '{}' does not resolve",
                                name, mat_name);
            } else {
                auto result = resources.load_material_asset(mat_path, project);
                if (result) {
                    mat_handle = *result;
                } else {
                    FJELL_CORE_WARN("Object '{}': {}", name, result.error());
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
