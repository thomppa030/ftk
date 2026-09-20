#include "core/log.hpp"
#include "environment/environment_resolver.hpp"

#include "environment/day_profile_asset.hpp"
#include "environment/weather_player.hpp"
#include "physics/physics_types.hpp"
#include "renderer/components/motion_source_component.hpp"
#include "scene/light_node.hpp"
#include "scene/scene.hpp"

#include <cmath>

namespace fjell {

namespace fog_group_track {

std::string_view group_of(std::string_view track_name, std::string_view prefix) {
    if (!track_name.starts_with(prefix)) return {};
    return track_name.substr(prefix.size());
}

} // namespace fog_group_track

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

// A body's light on the ground: its intensity once the horizon fade has
// taken the part below the ground away.
float ground_intensity(const DirectionalBody& body) {
    return body.intensity * horizon_fade(body.elevation);
}

} // namespace

WorldEnvironmentNode* find_world_environment(Scene& scene) {
    for (const auto& obj : scene.objects()) {
        if (auto* env = dynamic_cast<WorldEnvironmentNode*>(obj.get())) return env;
    }
    return nullptr;
}

const WorldEnvironmentNode* find_world_environment(const Scene& scene) {
    for (const auto& obj : scene.objects()) {
        if (const auto* env = dynamic_cast<const WorldEnvironmentNode*>(obj.get())) return env;
    }
    return nullptr;
}

bool moon_leads(const DirectionalBody& sun, const DirectionalBody& moon, bool moon_led) {
    // The leader keeps the role until the other body outshines it by a clear
    // margin, so the two never trade places every frame around twilight.
    constexpr float HANDOFF_MARGIN = 1.25f;
    const float sun_light = ground_intensity(sun);
    const float moon_light = ground_intensity(moon);
    if (moon_light <= 0.0f) return false;
    if (moon_led) return moon_light * HANDOFF_MARGIN >= sun_light;
    return moon_light > sun_light * HANDOFF_MARGIN;
}

namespace {

// Blend everything a day profile shapes from `from` toward `to`.
void blend_profiled(EnvironmentState& from, const EnvironmentState& to, float t) {
    from.sun.color = glm::mix(from.sun.color, to.sun.color, t);
    from.sun.intensity = glm::mix(from.sun.intensity, to.sun.intensity, t);
    from.moon.color = glm::mix(from.moon.color, to.moon.color, t);
    from.moon.intensity = glm::mix(from.moon.intensity, to.moon.intensity, t);
    from.ambient_color = glm::mix(from.ambient_color, to.ambient_color, t);
    from.ambient_intensity = glm::mix(from.ambient_intensity, to.ambient_intensity, t);
    from.sky.top_color = glm::mix(from.sky.top_color, to.sky.top_color, t);
    from.sky.bottom_color = glm::mix(from.sky.bottom_color, to.sky.bottom_color, t);
    from.fog.color = glm::mix(from.fog.color, to.fog.color, t);
    from.fog.density = glm::mix(from.fog.density, to.fog.density, t);
    from.exposure_bias = glm::mix(from.exposure_bias, to.exposure_bias, t);
    from.star_intensity = glm::mix(from.star_intensity, to.star_intensity, t);
    if (t >= 0.5f) from.sky_uniforms = to.sky_uniforms;
}

} // namespace

EnvironmentState resolve_environment(const Scene& scene, bool moon_led,
                                    const WeatherSkyOverride* weather_sky_override, float dt) {
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
                f.motion_response = fog->motion_response;
                f.density_field_debug = fog->density_field_debug;
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
                fog_found = true;
                continue;
            }
        }
        if (const auto* volume = dynamic_cast<const FogVolumeNode*>(obj.get())) {
            state.fog_volumes.push_back(FogVolume{
                .transform = volume->world_matrix_,
                .color = volume->fog_color,
                .density = volume->density,
                .scattering_mult = volume->scattering_mult,
                .edge_falloff = volume->edge_falloff,
                .shape = volume->shape,
                .group = volume->fog_group,
                .noise_contrast = volume->noise_contrast,
                .noise_scale = volume->noise_scale,
                .noise_drift = volume->noise_drift_,
                .noise_octaves = volume->noise_octaves,
                .motion_response = volume->motion_response,
                .vortex_strength = volume->vortex_strength,
                .compression = volume->compression,
                .density_field_debug = volume->density_field_debug,
            });
            continue;
        }
        // Anything carrying a motion source writes into the motion field,
        // which is what everything reacting to movement reads. A character
        // gets one for free, because a capsule that walks is the common case
        // and its own size answers both questions the source asks; anything
        // else stirs the world by being given the component.
        //
        // A source standing still is still a source. Leaving one out would
        // mean the ground under someone's feet is unmarked until they move, so
        // a first step begins a stride away from where they were standing.
        if (const auto* spatial = obj->as_spatial()) {
            const auto* explicit_source = obj->components.get<MotionSourceComponent>();
            const auto* character = obj->components.get<CharacterComponent>();
            if (explicit_source != nullptr || character != nullptr) {
                // Where the node's own size answers, when the component does
                // not: a capsule's girth and how far it stands above its feet.
                const float default_radius = character != nullptr ? character->capsule_radius : 0.5f;
                const float default_height =
                    character == nullptr ? 1.8f
                    : (character->feet_at_origin ? character->capsule_height
                                                 : character->capsule_height * 0.5f);

                MotionSource source;
                source.node_id = obj->id;
                source.position = glm::vec3(spatial->world_matrix_[3]);
                // A character states its velocity; anything else is measured
                // by where it was. `prev_world_matrix_` is kept for motion
                // vectors and costs nothing to read, so a rigid body, an
                // animated prop and a scripted mover all stir the world
                // without physics having to reach into this.
                source.velocity = glm::vec3(0.0f);
                if (character != nullptr) {
                    source.velocity = character->velocity;
                } else if (dt > 0.0f) {
                    source.velocity = (glm::vec3(spatial->world_matrix_[3]) -
                                       glm::vec3(spatial->prev_world_matrix_[3])) / dt;
                }
                source.radius = explicit_source != nullptr && explicit_source->radius > 0.0f
                                    ? explicit_source->radius : default_radius;
                source.height = explicit_source != nullptr && explicit_source->height > 0.0f
                                    ? explicit_source->height : default_height;
                source.strength = explicit_source != nullptr ? explicit_source->strength : 1.0f;
                state.motion_sources.push_back(source);
            }
        }

        // Volumes are a list, so the walk cannot stop until it has seen every
        // node; the singular nodes above keep their own found flags.
    }

    const auto* env = find_world_environment(scene);
    const bool clock_runs = env != nullptr && env->time_of_day_enabled;
    if (clock_runs) {
        // The sun follows the path for the hour; its authored colour,
        // intensity and ambient stay in charge of the look.
        const SunPath path{.tilt_degrees = env->sun_path_tilt,
                           .north_yaw_degrees = env->north_yaw,
                           .noon_hour = env->noon_hour};
        state.hour = wrap_hour(env->time_of_day);
        state.sun.direction = sun_direction(path, state.hour);
        state.sun.elevation = elevation_of(state.sun.direction);
        state.phase = time_phase(state.sun.elevation, state.hour, env->noon_hour);
        state.celestial_axis = sun_path_axis(path);
        state.celestial_angle = sun_path_angle(path, state.hour);

        // The moon follows the same path, placed by its phase: the phase is
        // the angle between the two bodies, so a quarter phase is six hours
        // along the path and a full moon is opposite the sun. Deriving the
        // position this way is what keeps the lit shape and the brightness
        // telling the same story — moon_disc() shades a sphere by where the
        // sun really is, so a position set independently would draw a shape
        // the phase disagreed with.
        state.moon.direction = sun_direction(path, state.hour + env->moon_phase * 24.0f);
        state.moon.elevation = elevation_of(state.moon.direction);
        state.moon.color = env->moon_color;
        state.moon.intensity = env->moon_intensity * moon_phase_brightness(env->moon_phase);
        state.moon_phase = env->moon_phase;

        // Stars come out as the sun goes down: none while it is up, all of
        // them once it is well below the horizon. A profile's star track
        // shapes this further.
        state.star_intensity = 1.0f - glm::smoothstep(-0.15f, 0.0f, std::sin(state.sun.elevation));
    }

    if (env != nullptr) {
        // The node's sky uniform overrides first, then the profile, whose
        // custom tracks replace an override of the same name.
        for (const auto& u : env->sky_uniforms) {
            state.sky_uniforms.push_back(NamedValue{.name = u.name, .count = u.count, .values = u.values});
        }
        const WeatherSkyOverride* wso = weather_sky_override;
        const bool weather_sky = wso != nullptr && wso->weight > 0.0f &&
                                 (wso->profile != nullptr || !wso->sky_shader.empty());
        if (weather_sky) {
            // The weather's own look, faded in over the scene's: both
            // profiles start from the same authored values.
            EnvironmentState theirs = state;
            if (env->day_profile_asset != nullptr) env->day_profile_asset->apply(state.hour, state);
            if (wso->profile != nullptr) wso->profile->apply(state.hour, theirs);
            else if (env->day_profile_asset != nullptr) env->day_profile_asset->apply(state.hour, theirs);
            blend_profiled(state, theirs, wso->weight);
            if (!wso->sky_shader.empty() && wso->weight >= 0.5f) state.sky.sky_shader = wso->sky_shader;
        } else if (env->day_profile_asset != nullptr) {
            env->day_profile_asset->apply(state.hour, state);
        }
    }

    if (clock_runs) {
        // Light fades out across the horizon so night is dark without anyone
        // turning the sun down; after the profile, so a profile that keeps
        // the sun bright at dusk still loses it once it has set.
        state.sun.intensity *= horizon_fade(state.sun.elevation);
        state.moon.intensity *= horizon_fade(state.moon.elevation);
    }

    // One body casts the shadows and fills the global UBO's directional
    // light; the other is lit without cascades.
    const bool moon_first = moon_leads(state.sun, state.moon, moon_led);
    state.primary = moon_first ? state.moon : state.sun;
    state.secondary = moon_first ? state.sun : state.moon;
    return state;
}

} // namespace fjell
