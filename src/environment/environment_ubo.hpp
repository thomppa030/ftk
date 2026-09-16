#pragma once

#include "environment/environment_state.hpp"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>

namespace fjell {

// ── Environment UBO ──────────────────────────────────────────────────────
//
// The GPU's view of the environment: global binding "environment_ubo",
// declared in shaders through FJELL_DECLARE_ENVIRONMENT_UBO in
// shaders/include/fjell_environment.glsl. Both sides describe the same block,
// so a field added here is added to the macro in the same change and the
// static_asserts below hold the offsets to what the GLSL std140 layout gives
// them. The block carries every slot the environment can drive so its layout
// does not shift as features fill the slots in.

struct EnvironmentUbo {
    alignas(16) glm::vec4 sun_dir_intensity;        // xyz toward the sun, w intensity
    alignas(16) glm::vec4 sun_color;                // rgb linear
    alignas(16) glm::vec4 moon_dir_intensity;
    alignas(16) glm::vec4 moon_color;                // rgb linear, w moon phase 0..1
    alignas(16) glm::vec4 secondary_dir_intensity;  // the body lit without cascaded shadows
    alignas(16) glm::vec4 secondary_color;
    alignas(16) glm::vec4 wind_dir_speed_gust;      // xyz direction, w speed × gust
    alignas(16) glm::vec4 wind_drift;               // xy metres the wind has carried things on the ground plane
    alignas(16) glm::vec4 weather0;                 // x cloud coverage, y cloud density, z precipitation, w precipitation type
    alignas(16) glm::vec4 weather1;                 // x wetness, y snow, z lightning ramp, w time of day in hours
    alignas(16) glm::vec4 front;                    // xy front direction on the xz plane, z edge position, w edge softness
    alignas(16) glm::vec4 sky_tint_star;            // rgb sky tint (1 = none), w star intensity
    alignas(16) glm::vec4 celestial_axis;           // xyz the sky's rotation axis, w radians turned since noon
    alignas(16) glm::mat4 cloud_shadow_vp;
    alignas(16) glm::mat4 overhead_vp;
    uint32_t cloud_shadow_texture{0};               // bindless index, 0 = none
    uint32_t overhead_depth_texture{0};
    uint32_t ubo_pad[2]{};

    bool operator==(const EnvironmentUbo&) const = default;
};

static_assert(offsetof(EnvironmentUbo, sun_dir_intensity) == 0);
static_assert(offsetof(EnvironmentUbo, sun_color) == 16);
static_assert(offsetof(EnvironmentUbo, moon_dir_intensity) == 32);
static_assert(offsetof(EnvironmentUbo, moon_color) == 48);
static_assert(offsetof(EnvironmentUbo, secondary_dir_intensity) == 64);
static_assert(offsetof(EnvironmentUbo, secondary_color) == 80);
static_assert(offsetof(EnvironmentUbo, wind_dir_speed_gust) == 96);
static_assert(offsetof(EnvironmentUbo, wind_drift) == 112);
static_assert(offsetof(EnvironmentUbo, weather0) == 128);
static_assert(offsetof(EnvironmentUbo, weather1) == 144);
static_assert(offsetof(EnvironmentUbo, front) == 160);
static_assert(offsetof(EnvironmentUbo, sky_tint_star) == 176);
static_assert(offsetof(EnvironmentUbo, celestial_axis) == 192);
static_assert(offsetof(EnvironmentUbo, cloud_shadow_vp) == 208);
static_assert(offsetof(EnvironmentUbo, overhead_vp) == 272);
static_assert(offsetof(EnvironmentUbo, cloud_shadow_texture) == 336);
static_assert(offsetof(EnvironmentUbo, overhead_depth_texture) == 340);
static_assert(sizeof(EnvironmentUbo) == 352);

/// Pack the resolved environment for upload.
[[nodiscard]] inline EnvironmentUbo make_environment_ubo(const EnvironmentState& env) {
    EnvironmentUbo ubo{};
    ubo.sun_dir_intensity = glm::vec4(env.sun.direction, env.sun.intensity);
    ubo.sun_color = glm::vec4(env.sun.color, 0.0f);
    ubo.moon_dir_intensity = glm::vec4(env.moon.direction, env.moon.intensity);
    ubo.moon_color = glm::vec4(env.moon.color, env.moon_phase);
    ubo.secondary_dir_intensity = glm::vec4(env.secondary.direction, env.secondary.intensity);
    ubo.secondary_color = glm::vec4(env.secondary.color, 0.0f);
    const WeatherParams& w = env.weather;
    ubo.wind_dir_speed_gust = glm::vec4(w.wind_direction, w.wind_speed * w.wind_gust);
    ubo.wind_drift = glm::vec4(w.wind_drift, 0.0f, 0.0f);
    ubo.weather0 = glm::vec4(w.cloud_coverage, w.cloud_density, w.precipitation,
                             static_cast<float>(w.precipitation_type));
    ubo.weather1 = glm::vec4(w.wetness, w.snow, w.lightning, env.hour);
    ubo.front = glm::vec4(w.front_direction, w.front_edge, w.front_softness);
    ubo.sky_tint_star = glm::vec4(w.sky_tint, env.star_intensity);
    ubo.celestial_axis = glm::vec4(env.celestial_axis, env.celestial_angle);
    ubo.cloud_shadow_vp = glm::mat4(1.0f);
    ubo.overhead_vp = glm::mat4(1.0f);
    return ubo;
}

} // namespace fjell
