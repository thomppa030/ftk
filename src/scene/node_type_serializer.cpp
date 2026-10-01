// What a typed node (camera, light, fog, post process volume, world
// environment) holds of its own, to and from JSON. The scene file writes and
// reads it through here, and so does undo of an edit to those settings.

#include "scene/node_serializer.hpp"

#include "environment/material_track.hpp"
#include "environment/weather_asset.hpp"
#include "ftk/base/log.hpp"
#include "ftk/math/color_space.hpp"
#include "scene/camera_node.hpp"
#include "scene/light_node.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace fjell {

nlohmann::json color_to_json(glm::vec3 linear) {
    const glm::vec3 c = ftk::color_space::linear_to_srgb(linear);
    return {c.x, c.y, c.z};
}

glm::vec3 color_from_json(const nlohmann::json& srgb) {
    return ftk::color_space::srgb_to_linear(
        glm::vec3{srgb[0].get<float>(), srgb[1].get<float>(), srgb[2].get<float>()});
}

// A material-carrying node's own values for its material's uniforms, under
// `params`, written as the picker shows them: colours in sRGB. Nothing is
// written when the node has none.
static void write_node_values(const std::vector<NamedValue>& params, nlohmann::json& block) {
    if (params.empty()) return;
    auto& jp = block["params"] = nlohmann::json::object();
    for (const auto& p : params) {
        auto values = nlohmann::json::array();
        for (uint32_t i = 0; i < p.count; ++i) values.push_back(p.values[i]);
        jp[p.name] = values;
    }
}

static std::vector<NamedValue> read_node_values(const nlohmann::json& block) {
    std::vector<NamedValue> params;
    if (!block.contains("params") || !block["params"].is_object()) return params;
    for (const auto& [param_name, values] : block["params"].items()) {
        if (!values.is_array() || values.empty()) continue;
        NamedValue p;
        p.name = param_name;
        p.count = static_cast<uint32_t>(std::min<size_t>(values.size(), 4));
        for (uint32_t i = 0; i < p.count; ++i) p.values[i] = values[i].get<float>();
        params.push_back(std::move(p));
    }
    return params;
}

// The settings of a node's own type, into the node's JSON: the node_type
// key and the keys that type's settings live under.
static void write_node_type(const Node& node, nlohmann::json& j) {
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
            {"max_distance", vf->max_distance},
            {"temporal_blend", vf->temporal_blend},
            {"motion_response", vf->motion_response},
            {"density_field_debug", vf->density_field_debug},
            {"shafts_enabled", vf->shafts_enabled},
            {"shaft_intensity", vf->shaft_intensity},
            {"shaft_density_mult", vf->shaft_density_mult},
            {"shaft_max_reach", vf->shaft_max_reach},
            {"shaft_step_count", vf->shaft_step_count},
            {"shaft_anisotropy", vf->shaft_anisotropy},
            {"shaft_directionality", vf->shaft_directionality},
            {"shaft_threshold", vf->shaft_threshold}
        };
        // The look is the material; the node's own values for its uniforms
        // are written as the picker shows them, colours in sRGB.
        if (!vf->material_path.empty()) {
            j["fog"]["material"] = vf->material_path;
        }
        write_node_values(vf->params, j["fog"]);
    } else if (auto* fv = dynamic_cast<const FogVolumeNode*>(&node)) {
        j["node_type"] = "fog_volume";
        j["fog_volume"] = {
            {"shape", static_cast<int>(fv->shape)},
            {"group", fv->fog_group},
            {"edge_falloff", fv->edge_falloff},
            {"drift_direction", {fv->drift_direction.x, fv->drift_direction.y, fv->drift_direction.z}},
            {"drift_speed", fv->drift_speed},
            {"wind_influence", fv->wind_influence},
            {"wind_speed_scale", fv->wind_speed_scale},
            {"motion_response", fv->motion_response},
            {"vortex_strength", fv->vortex_strength},
            {"compression", fv->compression},
            {"density_field_debug", fv->density_field_debug}
        };
        if (!fv->material_path.empty()) {
            j["fog_volume"]["material"] = fv->material_path;
        }
        write_node_values(fv->params, j["fog_volume"]);
    } else if (auto* pv = dynamic_cast<const PostProcessVolumeNode*>(&node)) {
        j["node_type"] = "post_process_volume";
        j["post_volume"] = {
            {"shape", static_cast<int>(pv->shape)},
            {"edge_falloff", pv->edge_falloff},
            {"infinite_bounds", pv->infinite_bounds},
            {"priority", pv->priority},
            {"weight", pv->weight},
            {"group", pv->post_group}
        };
        if (!pv->material_path.empty()) {
            j["post_volume"]["material"] = pv->material_path;
        }
        write_node_values(pv->params, j["post_volume"]);
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
            if (env->weather_start_phase != WeatherPhase::Approach) {
                j["weather_start_phase"] = weather_phase_name(env->weather_start_phase);
            }
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
            {"moon_phase", env->moon_phase},
            {"moon_color", color_to_json(env->moon_color)},
            {"moon_intensity", env->moon_intensity}
        };
    }
}

nlohmann::json serialize_node_type(const Node& node) {
    nlohmann::json j = nlohmann::json::object();
    write_node_type(node, j);
    return j;
}

// Sets a typed node's own settings from its JSON, as write_node_type wrote
// them. Works on a node that already holds settings, which it replaces, so an
// edit can be undone by applying what was there before.
bool apply_node_type(Node& node, const nlohmann::json& j) {
    if (auto* cam = dynamic_cast<CameraNode*>(&node)) {
        if (j.contains("camera")) {
            auto& jc = j["camera"];
            cam->fov = jc.value("fov", 45.0f);
            cam->near_plane = jc.value("near", 0.1f);
            cam->far_plane = jc.value("far", 1000.0f);
            cam->projection = static_cast<CameraProjection>(jc.value("projection", 0));
            cam->ortho_size = jc.value("ortho_size", 10.0f);
        }
        return true;
    } else if (auto* dl = dynamic_cast<DirectionalLightNode*>(&node)) {
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
        return true;
    } else if (auto* pl = dynamic_cast<PointLightNode*>(&node)) {
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
        return true;
    } else if (auto* sl = dynamic_cast<SpotLightNode*>(&node)) {
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
        return true;
    } else if (auto* vf = dynamic_cast<VolumetricFogNode*>(&node)) {
        if (j.contains("fog")) {
            auto& jf = j["fog"];
            vf->material_path = jf.value("material", std::string{});
            vf->params = read_node_values(jf);
            // A scene from before the fog had a material carries its
            // look as fields. They become the node's own values for the
            // default shader's uniforms of the same names, so the haze
            // draws as it did; the next save writes them as params. The
            // colour was saved in sRGB, which is how a param holds one.
            for (const char* key : {"density", "height_falloff", "scattering", "anisotropy"}) {
                if (!jf.contains(key) || find_named(vf->params, key) != nullptr) continue;
                vf->params.push_back(NamedValue{.name = key, .count = 1,
                                                .values = {jf.value(key, 0.0f)}});
            }
            if (jf.contains("color") && jf["color"].is_array()
                && find_named(vf->params, material_track::FOG_COLOR_UNIFORM) == nullptr) {
                const auto& c = jf["color"];
                vf->params.push_back(NamedValue{
                    .name = std::string(material_track::FOG_COLOR_UNIFORM), .count = 3,
                    .values = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>()},
                    .srgb = true});
            }
            vf->max_distance = jf.value("max_distance", 130.0f);
            vf->temporal_blend = jf.value("temporal_blend", 0.9f);
            vf->motion_response = jf.value("motion_response", 0.0f);
            vf->density_field_debug = jf.value("density_field_debug", 0.0f);
            vf->shafts_enabled = jf.value("shafts_enabled", true);
            vf->shaft_intensity = jf.value("shaft_intensity", 1.0f);
            vf->shaft_density_mult = jf.value("shaft_density_mult", 1.0f);
            vf->shaft_max_reach = jf.value("shaft_max_reach", 200.0f);
            vf->shaft_step_count = jf.value("shaft_step_count", 32);
            vf->shaft_anisotropy = jf.value("shaft_anisotropy", 0.75f);
            vf->shaft_directionality = jf.value("shaft_directionality", 1.0f);
            vf->shaft_threshold = jf.value("shaft_threshold", 0.0f);
            // A scene may still carry `shaft_cone_inner`/`_outer` (or the
            // older `shaft_halo_*`). The cone is gone — the open-sky fade
            // asks whether the ray met an occluder instead of guessing
            // from the angle to the sun — so the keys are read past and
            // dropped on the next save.
        }
        return true;
    } else if (auto* fv = dynamic_cast<FogVolumeNode*>(&node)) {
        if (j.contains("fog_volume")) {
            auto& jv = j["fog_volume"];
            fv->shape = static_cast<ColliderShape>(jv.value("shape", 0));
            fv->fog_group = jv.value("group", std::string{});
            fv->edge_falloff = jv.value("edge_falloff", 0.2f);
            fv->material_path = jv.value("material", std::string{});
            fv->params = read_node_values(jv);
            // A scene from before the volume had a material carries its
            // look as fields. They become the node's own values for the
            // default shader's uniforms, under the names those have, so
            // the bank draws as it did; the next save writes them as
            // params. The colour was saved in sRGB, as a param holds one.
            const std::pair<const char*, const char*> legacy[] = {
                {"density", "density"}, {"scattering_mult", "scattering"},
                {"noise_contrast", "noise_contrast"}, {"noise_scale", "noise_scale"},
                {"noise_octaves", "noise_octaves"},
            };
            for (const auto& [key, uniform] : legacy) {
                if (!jv.contains(key) || find_named(fv->params, uniform) != nullptr) continue;
                fv->params.push_back(NamedValue{.name = uniform, .count = 1,
                                                .values = {jv[key].get<float>()}});
            }
            if (jv.contains("color") && jv["color"].is_array()
                && find_named(fv->params, material_track::FOG_COLOR_UNIFORM) == nullptr) {
                const auto& c = jv["color"];
                fv->params.push_back(NamedValue{
                    .name = std::string(material_track::FOG_COLOR_UNIFORM), .count = 3,
                    .values = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>()},
                    .srgb = true});
            }
            // The drift was the noise's before the noise moved into the
            // shader, and before that one vector carrying both heading
            // and pace: its direction is the direction and its length
            // the speed.
            const char* dir_key = jv.contains("drift_direction") ? "drift_direction"
                                                                 : "noise_direction";
            if (jv.contains(dir_key) && jv[dir_key].is_array()) {
                auto& d = jv[dir_key];
                fv->drift_direction = {d[0].get<float>(), d[1].get<float>(), d[2].get<float>()};
            }
            const char* speed_key = jv.contains("drift_speed") ? "drift_speed" : "noise_speed";
            if (jv.contains(speed_key) && jv[speed_key].is_array()) {
                auto& s = jv[speed_key];
                const glm::vec3 drift{s[0].get<float>(), s[1].get<float>(), s[2].get<float>()};
                fv->drift_speed = glm::length(drift);
                if (fv->drift_speed > 1e-4f) fv->drift_direction = drift / fv->drift_speed;
            } else {
                fv->drift_speed = jv.value(speed_key, 0.3f);
            }
            fv->wind_influence = jv.value("wind_influence", 0.0f);
            fv->wind_speed_scale = jv.value("wind_speed_scale", 0.15f);
            fv->motion_response = jv.value("motion_response", 0.0f);
            fv->vortex_strength = jv.value("vortex_strength", 0.0f);
            fv->compression = jv.value("compression", 0.0f);
            fv->density_field_debug = jv.value("density_field_debug", 0.0f);
        }
        return true;
    } else if (auto* pv = dynamic_cast<PostProcessVolumeNode*>(&node)) {
        if (j.contains("post_volume")) {
            auto& jv = j["post_volume"];
            pv->shape = static_cast<ColliderShape>(jv.value("shape", 0));
            pv->edge_falloff = jv.value("edge_falloff", 0.2f);
            pv->infinite_bounds = jv.value("infinite_bounds", false);
            pv->priority = jv.value("priority", 0);
            pv->weight = jv.value("weight", 1.0f);
            pv->post_group = jv.value("group", std::string{});
            pv->material_path = jv.value("material", std::string{});
            pv->params = read_node_values(jv);
        }
        return true;
    } else if (auto* env = dynamic_cast<WorldEnvironmentNode*>(&node)) {
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
            env->hdri_path = js.value("hdri_path", std::string{});
            env->sky_shader = js.value("sky_shader", std::string{});
        }
        env->day_profile = j.value("day_profile", std::string{});
        env->weather = j.value("weather", std::string{});
        env->weather_start_phase = WeatherPhase::Approach;
        if (j.contains("weather_start_phase")) {
            const auto phase_name = j.value("weather_start_phase", std::string{});
            if (const auto phase = weather_phase_from_name(phase_name)) {
                env->weather_start_phase = *phase;
            } else {
                FTK_CORE_WARN("World environment: '{}' is not a weather phase, "
                                "starting at the approach", phase_name);
            }
        }
        env->climate = j.value("climate", std::string{});
        env->sky_uniforms.clear();
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
            env->moon_phase = jt.value("moon_phase", 0.5f);
            // Scenes written when the moon had its own offset carry a
            // phase that only set brightness. The offset is what actually
            // shaped the disc, so it is what the phase becomes.
            if (jt.contains("moon_offset_hours") && !jt.contains("moon_phase")) {
                env->moon_phase = jt.value("moon_offset_hours", 12.0f) / 24.0f;
            }
            if (jt.contains("moon_color")) env->moon_color = color_from_json(jt["moon_color"]);
            env->moon_intensity = jt.value("moon_intensity", 0.08f);
        }
        return true;
    }
    return false;
}

} // namespace fjell
