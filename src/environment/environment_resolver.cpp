#include "environment/environment_resolver.hpp"

#include "scene/light_node.hpp"
#include "scene/scene.hpp"

#include <cmath>

namespace fjell {

namespace {

// The sun a scene gets when it has no directional light: a warm key light
// from up and to the side, bright enough to read the geometry. The same
// values a fresh DirectionalLightNode starts with, except for the ambient
// strength, which a lit scene without an authored light keeps modest.
constexpr glm::vec3 DEFAULT_SUN_TRAVEL{-0.3f, -1.0f, -0.5f};
constexpr glm::vec3 DEFAULT_SUN_COLOR{1.0f, 1.0f, 0.95f};
constexpr float DEFAULT_SUN_INTENSITY = 1.5f;
constexpr glm::vec3 DEFAULT_AMBIENT_COLOR{0.1f, 0.1f, 0.12f};
constexpr float DEFAULT_AMBIENT_INTENSITY = 0.4f;

DirectionalBody body_from_travel(const glm::vec3& travel, const glm::vec3& color, float intensity) {
    DirectionalBody body;
    body.direction = glm::normalize(-travel);
    body.color = color;
    body.intensity = intensity;
    body.elevation = std::asin(glm::clamp(body.direction.y, -1.0f, 1.0f));
    return body;
}

} // namespace

EnvironmentState resolve_environment(const Scene& scene) {
    EnvironmentState state;
    state.sun = body_from_travel(DEFAULT_SUN_TRAVEL, DEFAULT_SUN_COLOR, DEFAULT_SUN_INTENSITY);
    state.ambient_color = DEFAULT_AMBIENT_COLOR;
    state.ambient_intensity = DEFAULT_AMBIENT_INTENSITY;

    bool sun_found = false;
    bool env_found = false;
    bool fog_found = false;
    for (const auto& obj : scene.objects()) {
        if (!sun_found) {
            if (const auto* dl = dynamic_cast<const DirectionalLightNode*>(obj.get())) {
                state.sun = body_from_travel(dl->direction, dl->color, dl->intensity);
                state.ambient_color = dl->ambient_color;
                state.ambient_intensity = dl->ambient_intensity;
                sun_found = true;
                continue;
            }
        }
        if (!env_found) {
            if (const auto* env = dynamic_cast<const WorldEnvironmentNode*>(obj.get())) {
                auto& sky = state.sky;
                sky.top_color = env->sky_top_color;
                sky.bottom_color = env->sky_bottom_color;
                sky.ibl_intensity = env->ibl_intensity;
                sky.hdri_path = env->hdri_path;
                sky.sky_shader = env->sky_shader;
                sky.atmosphere_enabled = env->atmosphere_enabled;
                sky.atmosphere_rayleigh_scale = env->atmosphere_rayleigh_scale;
                sky.atmosphere_mie_scale = env->atmosphere_mie_scale;
                sky.atmosphere_mie_g = env->atmosphere_mie_g;
                sky.atmosphere_mie_coeff = env->atmosphere_mie_coeff;
                sky.atmosphere_sun_intensity = env->atmosphere_sun_intensity;
                sky.atmosphere_hdri_floor = env->atmosphere_hdri_floor;

                auto& cs = state.contact_shadows;
                cs.enabled = env->contact_shadows_enabled;
                cs.distance = env->contact_shadow_distance;
                cs.thickness = env->contact_shadow_thickness;
                cs.steps = env->contact_shadow_steps;
                env_found = true;
                continue;
            }
        }
        if (!fog_found) {
            if (const auto* fog = dynamic_cast<const VolumetricFogNode*>(obj.get())) {
                auto& f = state.fog;
                f.enabled = true;
                f.density = fog->density;
                f.height = glm::vec3(fog->world_matrix_[3]).y;
                f.height_falloff = fog->height_falloff;
                f.scattering = fog->scattering;
                f.anisotropy = fog->anisotropy;
                f.max_distance = fog->max_distance;
                f.temporal_blend = fog->temporal_blend;
                f.color = fog->fog_color;

                auto& s = state.sun_shafts;
                s.enabled = fog->shafts_enabled;
                s.intensity = fog->shaft_intensity;
                s.density_mult = fog->shaft_density_mult;
                s.max_reach = fog->shaft_max_reach;
                s.step_count = fog->shaft_step_count;
                s.anisotropy = fog->shaft_anisotropy;
                s.directionality = fog->shaft_directionality;
                s.threshold = fog->shaft_threshold;
                s.halo_inner = fog->shaft_halo_inner;
                s.halo_outer = fog->shaft_halo_outer;
                fog_found = true;
                continue;
            }
        }
        if (sun_found && env_found && fog_found) break;
    }

    // A scene has one body, the sun: it casts the shadows and the secondary
    // slot stays dark.
    state.primary = state.sun;
    state.secondary = state.moon;
    return state;
}

} // namespace fjell
