#include "scene/node_serializer.hpp"
#include "scene/node.hpp"
#include "scene/camera_node.hpp"
#include "scene/light_node.hpp"
#include "ui/game_ui/ui_widget_component.hpp"
#include "animation/animation_clip.hpp"
#include "animation/animator_component.hpp"
#include "animation/skeleton.hpp"
#include "audio/audio_component.hpp"
#include "physics/physics_types.hpp"
#include "renderer/arrow_component.hpp"
#include "renderer/mesh_component.hpp"
#include "renderer/particle_emitter.hpp"
#include "renderer/vfx_asset.hpp"
#include "renderer/vfx_emitter.hpp"
#include "renderer/skeletal_mesh_component.hpp"
#include "renderer/sprite_desc.hpp"
#include "renderer/material.hpp"
#include "renderer/model.hpp"
#include "renderer/resource_manager.hpp"
#include "renderer/skeletal_model.hpp"
#include "renderer/texture.hpp"
#include "scripting/fjell/component.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#include <nlohmann/json.hpp>

namespace fjell {

// ── Serialize ────────────────────────────────────────────────────────────

nlohmann::json serialize_node(const Node& node) {
    nlohmann::json j;
    j["id"] = node.id;
    j["parent_id"] = node.parent_id;
    j["name"] = node.name;
    j["source"] = node.source;
    if (!node.preset_source.empty()) {
        j["preset_source"] = node.preset_source;
    }

    if (!node.is_spatial()) {
        j["node_type"] = "folder";
    }

    // Mesh component
    if (const auto* mesh = node.components.get<MeshComponent>()) {
        nlohmann::json jmesh;
        jmesh["material"] = mesh->material_name;
        if (mesh->material) {
            const auto& bcf = mesh->material->params().base_color_factor;
            if (bcf.r != 1.0f || bcf.g != 1.0f || bcf.b != 1.0f || bcf.a != 1.0f) {
                if (bcf.a < 1.0f) {
                    jmesh["color"] = {bcf.r, bcf.g, bcf.b, bcf.a};
                } else {
                    jmesh["color"] = {bcf.r, bcf.g, bcf.b};
                }
            }
        }
        j["mesh"] = jmesh;
    }

    // Skeletal mesh
    if (const auto* smc = node.components.get<SkeletalMeshComponent>()) {
        nlohmann::json jskel;
        jskel["skeleton"] = smc->skeleton_path;
        jskel["material"] = smc->material_name;
        if (!smc->clip_paths.empty()) {
            jskel["clips"] = smc->clip_paths;
        }
        if (!smc->animset_path.empty()) {
            jskel["animset"] = smc->animset_path;
        }
        j["skeletal_mesh"] = jskel;
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

    // Rigidbody
    if (const auto* rb = node.components.get<RigidbodyComponent>()) {
        j["rigidbody"] = {
            {"body_type", static_cast<int>(rb->body_type)},
            {"mass", rb->mass},
            {"friction", rb->friction},
            {"restitution", rb->restitution},
            {"linear_damping", rb->linear_damping},
            {"angular_damping", rb->angular_damping},
            {"gravity_enabled", rb->gravity_enabled}
        };
    }

    // Collider
    if (const auto* col = node.components.get<ColliderComponent>()) {
        j["collider"] = {
            {"shape", static_cast<int>(col->shape)},
            {"half_extents", {col->half_extents.x, col->half_extents.y, col->half_extents.z}},
            {"radius", col->radius},
            {"height", col->height},
            {"offset", {col->offset.x, col->offset.y, col->offset.z}},
            {"is_trigger", col->is_trigger}
        };
    }

    // Emitter / VFX
    if (const auto* em = node.components.get<EmitterComponent>()) {
        if (!em->vfx_path.empty()) {
            j["vfx"] = em->vfx_path;
        } else {
            j["emitter"] = {
                {"emission_rate", em->emission_rate},
                {"lifetime_min", em->lifetime_min},
                {"lifetime_max", em->lifetime_max},
                {"max_particles", em->max_particles},
                {"velocity_min", {em->velocity_min.x, em->velocity_min.y, em->velocity_min.z}},
                {"velocity_max", {em->velocity_max.x, em->velocity_max.y, em->velocity_max.z}},
                {"gravity", {em->gravity.x, em->gravity.y, em->gravity.z}},
                {"color_start", {em->color_start.r, em->color_start.g, em->color_start.b, em->color_start.a}},
                {"color_end", {em->color_end.r, em->color_end.g, em->color_end.b, em->color_end.a}},
                {"size_start", em->size_start},
                {"size_end", em->size_end},
                {"blend_mode", static_cast<int>(em->blend_mode)}
            };
        }
    }

    // Sprite
    if (const auto* sp = node.components.get<SpriteComponent>()) {
        j["sprite"] = {
            {"texture", sp->texture_path},
            {"tint", {sp->tint.r, sp->tint.g, sp->tint.b, sp->tint.a}},
            {"opacity", sp->opacity},
            {"editor_only", sp->editor_only}
        };
    }

    // Audio
    if (const auto* au = node.components.get<AudioSourceComponent>()) {
        j["audio"] = {
            {"clip", au->clip_path},
            {"volume", au->volume},
            {"pitch", au->pitch},
            {"min_distance", au->min_distance},
            {"max_distance", au->max_distance},
            {"loop", au->loop},
            {"spatial", au->spatial},
            {"play_on_start", au->play_on_start}
        };
    }

    // Arrow
    if (const auto* arr = node.components.get<ArrowComponent>()) {
        j["arrow"] = {
            {"length", arr->length},
            {"color", {arr->color.x, arr->color.y, arr->color.z}}
        };
    }

    // Character
    if (const auto* cc = node.components.get<CharacterComponent>()) {
        j["character"] = {
            {"capsule_radius", cc->capsule_radius},
            {"capsule_height", cc->capsule_height},
            {"max_slope_angle", cc->max_slope_angle},
            {"mass", cc->mass}
        };
    }

    // UI Widget
    if (const auto* wc = node.components.get<UiWidgetComponent>()) {
        nlohmann::json jw;
        jw["widget"] = wc->widget_path;
        if (wc->render_mode != UiWidgetComponent::RenderMode::screen_space) {
            jw["render_mode"] = static_cast<int>(wc->render_mode);
        }
        if (!wc->style_classes.empty()) jw["style_classes"] = wc->style_classes;
        if (wc->render_mode == UiWidgetComponent::RenderMode::world_space) {
            jw["pixels_per_unit"] = wc->pixels_per_unit;
            jw["billboard"] = wc->billboard;
            jw["virtual_viewport"] = {wc->virtual_viewport.x, wc->virtual_viewport.y};
            jw["offset"] = {wc->offset.x, wc->offset.y, wc->offset.z};
        }
        j["ui_widget"] = jw;
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
        j["camera"] = {
            {"fov", cam->fov},
            {"near", cam->near_plane},
            {"far", cam->far_plane}
        };
    } else if (auto* dl = dynamic_cast<const DirectionalLightNode*>(&node)) {
        j["node_type"] = "directional_light";
        j["light"] = {
            {"direction", {dl->direction.x, dl->direction.y, dl->direction.z}},
            {"color", {dl->color.x, dl->color.y, dl->color.z}},
            {"intensity", dl->intensity},
            {"ambient_color", {dl->ambient_color.x, dl->ambient_color.y, dl->ambient_color.z}},
            {"ambient_intensity", dl->ambient_intensity}
        };
    } else if (auto* pl = dynamic_cast<const PointLightNode*>(&node)) {
        j["node_type"] = "point_light";
        j["light"] = {
            {"color", {pl->color.x, pl->color.y, pl->color.z}},
            {"intensity", pl->intensity},
            {"radius", pl->radius}
        };
    } else if (auto* sl = dynamic_cast<const SpotLightNode*>(&node)) {
        j["node_type"] = "spot_light";
        j["light"] = {
            {"direction", {sl->direction.x, sl->direction.y, sl->direction.z}},
            {"color", {sl->color.x, sl->color.y, sl->color.z}},
            {"intensity", sl->intensity},
            {"radius", sl->radius},
            {"inner_angle", sl->inner_angle},
            {"outer_angle", sl->outer_angle}
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
            {"color", {vf->fog_color.r, vf->fog_color.g, vf->fog_color.b}}
        };
    } else if (auto* env = dynamic_cast<const WorldEnvironmentNode*>(&node)) {
        j["node_type"] = "world_environment";
        j["sky"] = {
            {"sky_top_color", {env->sky_top_color.x, env->sky_top_color.y, env->sky_top_color.z}},
            {"sky_bottom_color", {env->sky_bottom_color.x, env->sky_bottom_color.y, env->sky_bottom_color.z}},
            {"ibl_intensity", env->ibl_intensity}
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
                            [[maybe_unused]] ResourceManager& resources,
                            const Project* project) {
    if (j.contains("rigidbody")) {
        auto& jrb = j["rigidbody"];
        auto& rb = node.components.add<RigidbodyComponent>();
        rb.body_type = static_cast<BodyType>(jrb.value("body_type", 2));
        rb.mass = jrb.value("mass", 1.0f);
        rb.friction = jrb.value("friction", 0.5f);
        rb.restitution = jrb.value("restitution", 0.3f);
        rb.linear_damping = jrb.value("linear_damping", 0.05f);
        rb.angular_damping = jrb.value("angular_damping", 0.05f);
        rb.gravity_enabled = jrb.value("gravity_enabled", true);
    }

    if (j.contains("collider")) {
        auto& jcol = j["collider"];
        auto& col = node.components.add<ColliderComponent>();
        col.shape = static_cast<ColliderShape>(jcol.value("shape", 0));
        if (jcol.contains("half_extents")) {
            auto& he = jcol["half_extents"];
            col.half_extents = {he[0].get<float>(), he[1].get<float>(), he[2].get<float>()};
        }
        col.radius = jcol.value("radius", 0.5f);
        col.height = jcol.value("height", 1.0f);
        if (jcol.contains("offset")) {
            auto& o = jcol["offset"];
            col.offset = {o[0].get<float>(), o[1].get<float>(), o[2].get<float>()};
        }
        col.is_trigger = jcol.value("is_trigger", false);
    }

    if (j.contains("emitter")) {
        auto& jem = j["emitter"];
        auto& em = node.components.add<EmitterComponent>();
        em.emission_rate = jem.value("emission_rate", 50.0f);
        em.lifetime_min = jem.value("lifetime_min", 1.0f);
        em.lifetime_max = jem.value("lifetime_max", 3.0f);
        em.max_particles = jem.value("max_particles", 10000u);
        if (jem.contains("velocity_min")) {
            auto& v = jem["velocity_min"];
            em.velocity_min = {v[0].get<float>(), v[1].get<float>(), v[2].get<float>()};
        }
        if (jem.contains("velocity_max")) {
            auto& v = jem["velocity_max"];
            em.velocity_max = {v[0].get<float>(), v[1].get<float>(), v[2].get<float>()};
        }
        if (jem.contains("gravity")) {
            auto& g = jem["gravity"];
            em.gravity = {g[0].get<float>(), g[1].get<float>(), g[2].get<float>()};
        }
        if (jem.contains("color_start")) {
            auto& c = jem["color_start"];
            em.color_start = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c[3].get<float>()};
        }
        if (jem.contains("color_end")) {
            auto& c = jem["color_end"];
            em.color_end = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c[3].get<float>()};
        }
        em.size_start = jem.value("size_start", 0.1f);
        em.size_end = jem.value("size_end", 0.02f);
        em.blend_mode = static_cast<ParticleBlendMode>(jem.value("blend_mode", 0));
    }

    if (j.contains("vfx")) {
        std::string vfx_path = j["vfx"].get<std::string>();
        auto resolved = base_path::resolve_asset(vfx_path, project);
        try {
            auto effect = load_vfx_effect(resolved);
            auto& ec = node.components.get<EmitterComponent>()
                ? *node.components.get<EmitterComponent>()
                : node.components.add<EmitterComponent>();
            ec.effect = std::make_shared<VfxEffect>(std::move(effect));
            ec.vfx_path = vfx_path;
        } catch (const std::exception& e) {
            FJELL_CORE_ERROR("Failed to load VFX: {}", e.what());
        }
    }

    if (j.contains("sprite")) {
        auto& jsp = j["sprite"];
        auto& sp = node.components.add<SpriteComponent>();
        sp.texture_path = jsp.value("texture", "");
        if (sp.texture_path.starts_with("assets/icons/")) {
            sp.texture_path = "engine_assets/icons/" +
                sp.texture_path.substr(13);
        }
        if (jsp.contains("tint")) {
            auto& t = jsp["tint"];
            sp.tint = {t[0].get<float>(), t[1].get<float>(), t[2].get<float>(), t[3].get<float>()};
        }
        sp.opacity = jsp.value("opacity", 1.0f);
        sp.editor_only = jsp.value("editor_only", false);
    }

    if (j.contains("audio")) {
        auto& jau = j["audio"];
        auto& au = node.components.add<AudioSourceComponent>();
        au.clip_path = jau.value("clip", "");
        au.volume = jau.value("volume", 1.0f);
        au.pitch = jau.value("pitch", 1.0f);
        au.min_distance = jau.value("min_distance", 1.0f);
        au.max_distance = jau.value("max_distance", 50.0f);
        au.loop = jau.value("loop", false);
        au.spatial = jau.value("spatial", true);
        au.play_on_start = jau.value("play_on_start", false);
    }

    if (j.contains("arrow")) {
        auto& jar = j["arrow"];
        auto& arr = node.components.add<ArrowComponent>();
        arr.length = jar.value("length", 2.0f);
        if (jar.contains("color")) {
            auto& c = jar["color"];
            arr.color = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>()};
        }
    }

    if (j.contains("character")) {
        auto& jcc = j["character"];
        auto& cc = node.components.add<CharacterComponent>();
        cc.capsule_radius = jcc.value("capsule_radius", 0.3f);
        cc.capsule_height = jcc.value("capsule_height", 1.4f);
        cc.max_slope_angle = jcc.value("max_slope_angle", 50.0f);
        cc.mass = jcc.value("mass", 80.0f);
    }

    if (j.contains("ui_widget")) {
        auto& jw = j["ui_widget"];
        auto& wc = node.components.add<UiWidgetComponent>();
        wc.widget_path = jw.value("widget", "");
        wc.render_mode = static_cast<UiWidgetComponent::RenderMode>(jw.value("render_mode", 0));
        if (jw.contains("style_classes") && jw["style_classes"].is_array()) {
            for (const auto& cls : jw["style_classes"]) {
                wc.style_classes.push_back(cls.get<std::string>());
            }
        }
        wc.pixels_per_unit = jw.value("pixels_per_unit", 100.0f);
        wc.billboard = jw.value("billboard", true);
        if (jw.contains("virtual_viewport")) {
            auto& vv = jw["virtual_viewport"];
            wc.virtual_viewport = {vv[0].get<float>(), vv[1].get<float>()};
        }
        if (jw.contains("offset")) {
            auto& off = jw["offset"];
            wc.offset = {off[0].get<float>(), off[1].get<float>(), off[2].get<float>()};
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
                    dl->color = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>()};
                }
                dl->intensity = jl.value("intensity", 1.5f);
                if (jl.contains("ambient_color")) {
                    auto& c = jl["ambient_color"];
                    dl->ambient_color = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>()};
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
                    pl->color = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>()};
                }
                pl->intensity = jl.value("intensity", 1.0f);
                pl->radius = jl.value("radius", 10.0f);
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
                    sl->color = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>()};
                }
                sl->intensity = jl.value("intensity", 1.0f);
                sl->radius = jl.value("radius", 10.0f);
                sl->inner_angle = jl.value("inner_angle", 20.0f);
                sl->outer_angle = jl.value("outer_angle", 30.0f);
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
                    vf->fog_color = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>()};
                }
            }
            node = std::move(vf);
        } else if (node_type == "world_environment") {
            auto env = std::make_unique<WorldEnvironmentNode>();
            if (j.contains("sky")) {
                auto& js = j["sky"];
                if (js.contains("sky_top_color")) {
                    auto& c = js["sky_top_color"];
                    env->sky_top_color = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>()};
                }
                if (js.contains("sky_bottom_color")) {
                    auto& c = js["sky_bottom_color"];
                    env->sky_bottom_color = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>()};
                }
                env->ibl_intensity = js.value("ibl_intensity", 1.0f);
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
        std::string skel_rel = jsm.value("skeleton", "");

        auto skel_model_result = resources.load_skeletal_model(
            base_path::resolve_asset(source, project));
        if (!skel_model_result) return nullptr;

        auto skel_model_handle = *skel_model_result;
        auto* skel_model = resources.get(skel_model_handle);
        if (!skel_model) return nullptr;

        auto node = std::make_unique<Node3D>();
        node->name = name;
        node->source = source;
        node->transform = transform;
        node->id = loaded_id;
        node->parent_id = loaded_parent;

        auto& smc = node->components.add<SkeletalMeshComponent>();
        smc.skeletal_model_handle = skel_model_handle;
        smc.skeletal_model = skel_model;
        smc.material_name = jsm.value("material", "default");
        smc.skeleton_path = skel_rel;

        // Material
        auto mat_n = smc.material_name;
        if (mat_n.ends_with(".fjmat")) {
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

        // Skeleton
        if (!skel_rel.empty()) {
            try {
                smc.skeleton = std::make_shared<Skeleton>(
                    Skeleton::load(base_path::resolve_asset(skel_rel, project)));
            } catch (const std::exception& e) {
                FJELL_CORE_ERROR("Failed to load skeleton {}: {}", skel_rel, e.what());
                return nullptr;
            }
        }

        // Animator
        smc.animator = std::make_unique<AnimatorComponent>();
        if (smc.skeleton) {
            smc.animator->init(smc.skeleton.get());
        }

        // Animation clips
        if (jsm.contains("clips") && jsm["clips"].is_array()) {
            for (const auto& jc : jsm["clips"]) {
                std::string clip_rel = jc.get<std::string>();
                smc.clip_paths.push_back(clip_rel);
                try {
                    auto clip = std::make_shared<AnimationClip>(
                        AnimationClip::load(base_path::resolve_asset(clip_rel, project)));
                    smc.clips.push_back(clip);
                    if (smc.clips.size() == 1) {
                        smc.animator->play(clip.get());
                    }
                } catch (const std::exception& e) {
                    FJELL_CORE_ERROR("Failed to load clip {}: {}", clip_rel, e.what());
                }
            }
        }

        if (jsm.contains("animset")) {
            smc.animset_path = jsm["animset"].get<std::string>();
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
            auto result = resources.load_model(base_path::resolve_asset(source, project));
            if (result) model_handle = *result;
        }

        if (!model_handle.valid()) return nullptr;

        auto default_tex = resources.load_texture(
            base_path::resolve("engine_assets/textures/test.png")).value_or(TextureHandle{});

        MaterialHandle mat_handle;
        if (mat_name.ends_with(".fjmat")) {
            std::string mat_path = base_path::resolve_asset(mat_name, project);
            auto result = resources.load_material_asset(mat_path, project);
            if (result) mat_handle = *result;
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
        if (!mat_name.ends_with(".fjmat") &&
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
