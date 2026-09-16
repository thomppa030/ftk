#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <cmath>

namespace fjell {

// ── Sun path ─────────────────────────────────────────────────────────────
//
// Where the sun is at a given hour, in the terms an artist reasons about:
// the sun rises in the east, peaks at noon and sets in the west, twelve hours
// above the horizon, on a circle tilted away from the zenith. Tilt zero puts
// the noon sun straight overhead, ninety degrees keeps it circling the
// horizon. The world's east is +X and its north is -Z until `north_yaw`
// turns the whole path about the up axis.
//
// The moon and every other body follow the same circle from a different
// hour, so this is the one place the geometry lives.

struct SunPath {
    float tilt_degrees{40.0f};
    float north_yaw_degrees{0.0f};
    float noon_hour{12.0f};
};

/// Fold an hour into [0, 24).
[[nodiscard]] inline float wrap_hour(float hour) {
    float h = std::fmod(hour, 24.0f);
    return h < 0.0f ? h + 24.0f : h;
}

/// Unit direction toward the sun at `hour`.
[[nodiscard]] inline glm::vec3 sun_direction(const SunPath& path, float hour) {
    const float angle = (hour - path.noon_hour) / 24.0f * glm::two_pi<float>();
    // Untilted circle through the zenith: east at sunrise, overhead at noon,
    // west at sunset, underfoot at midnight.
    const glm::vec3 flat{-std::sin(angle), std::cos(angle), 0.0f};
    // Lean the circle toward the south (+Z) by the tilt.
    const float tilt = glm::radians(path.tilt_degrees);
    const glm::vec3 tilted{flat.x, flat.y * std::cos(tilt), flat.y * std::sin(tilt)};
    // Turn the compass.
    const float yaw = glm::radians(path.north_yaw_degrees);
    const float c = std::cos(yaw);
    const float s = std::sin(yaw);
    return glm::normalize(glm::vec3{tilted.x * c + tilted.z * s, tilted.y, -tilted.x * s + tilted.z * c});
}

/// Angle of a direction above the horizon, in radians; negative below it.
[[nodiscard]] inline float elevation_of(const glm::vec3& direction) {
    return std::asin(glm::clamp(direction.y, -1.0f, 1.0f));
}

/// How much of a body's light reaches the ground given its elevation: one
/// well above the horizon, zero once it has set, with a short fade across
/// the horizon so lighting never snaps. The band matches the atmosphere
/// shader's sun visibility so the sky and the ground agree on dusk.
[[nodiscard]] inline float horizon_fade(float elevation) {
    return glm::smoothstep(-0.1f, 0.05f, std::sin(elevation));
}

/// The part of the day a sun elevation and hour fall in.
enum class TimePhase : int { Night = 0, Dawn = 1, Day = 2, Dusk = 3 };

/// Twilight is the band within six degrees of the horizon either way: dawn
/// before noon, dusk after. Above it is day, below it night.
[[nodiscard]] inline TimePhase time_phase(float sun_elevation, float hour, float noon_hour) {
    constexpr float TWILIGHT = glm::radians(6.0f);
    if (sun_elevation > TWILIGHT) return TimePhase::Day;
    if (sun_elevation < -TWILIGHT) return TimePhase::Night;
    const float since_noon = wrap_hour(hour - noon_hour);
    return since_noon < 12.0f ? TimePhase::Dusk : TimePhase::Dawn;
}

} // namespace fjell
