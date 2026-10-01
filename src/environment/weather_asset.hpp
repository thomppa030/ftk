#pragma once

#include "environment/track_set.hpp"
#include "ftk/base/result.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace fjell {

// ── Weather event (.fjweather) ───────────────────────────────────────────
//
// A weather is an event with a timeline, not a preset. It moves through
// four phases: the approach, when the front is coming and the wind rises and
// the sky thickens from windward; the onset, when the front passes overhead
// and precipitation ramps; the sustain, the weather proper, which may last
// indefinitely for a steady state like a clear day; and the tail, when it
// tapers and the sky opens from the same side it closed from.
//
// Each phase has a duration range the director (or the player, on its own)
// rolls from, and the event's tracks are keyed in phase units: 0..1 is the
// approach, 1..2 the onset, 2..3 the sustain, 3..4 the tail, so a rolled
// duration stretches a phase without moving the keys inside it. A multiply
// track scales what the scene authors underneath, an override replaces it,
// and a track the event leaves out leaves the value alone.

// Also declared in the SDK's environment_api.hpp for scripts; the two are
// one type when both are compiled into the engine, so they agree down to
// the underlying type.
#ifndef FJELL_WEATHER_PHASE_DEFINED
#define FJELL_WEATHER_PHASE_DEFINED
enum class WeatherPhase : int { Approach = 0, Onset = 1, Sustain = 2, Tail = 3 };
#endif
inline constexpr uint32_t WEATHER_PHASE_COUNT = 4;

enum class PrecipitationType : uint8_t { None = 0, Rain = 1, Snow = 2 };

struct PhaseDuration {
    float min_seconds{60.0f};
    float max_seconds{120.0f};
    /// The phase never ends on its own; only a new event moves on from it.
    /// Only the sustain phase is expected to use this.
    bool indefinite{false};
};

/// The tracks the engine reads by name. Colour tracks are gradients, the
/// rest curves.
namespace weather_track {
inline constexpr std::string_view CLOUD_COVERAGE = "cloud_coverage";        // float 0..1, override
inline constexpr std::string_view CLOUD_DENSITY = "cloud_density";          // float 0..1, override
inline constexpr std::string_view PRECIPITATION = "precipitation";          // float 0..1, override
inline constexpr std::string_view WIND_SPEED = "wind_speed";                // float m/s, override
inline constexpr std::string_view WIND_GUST = "wind_gust";                  // float 0..1, override
inline constexpr std::string_view WIND_VEER = "wind_veer";                  // float degrees added to the wind's heading
inline constexpr std::string_view FOG_DENSITY = "fog_density";              // float, multiply or override
inline constexpr std::string_view FOG_COLOR = "fog_color";                  // color, multiply or override
inline constexpr std::string_view SUN_INTENSITY = "sun_intensity";          // float, multiply
inline constexpr std::string_view AMBIENT_INTENSITY = "ambient_intensity";  // float, multiply
inline constexpr std::string_view SKY_TINT = "sky_tint";                    // color, multiply
inline constexpr std::string_view LIGHTNING_RATE = "lightning_rate";        // float strikes per minute, override
inline constexpr std::string_view SKY_BLEND = "sky_blend";                  // float 0..1 toward the event's own sky / profile

[[nodiscard]] const std::vector<std::string_view>& all();
[[nodiscard]] TrackKind kind_of(std::string_view name);
[[nodiscard]] bool is_builtin(std::string_view name);
} // namespace weather_track

struct WeatherAsset {
    /// Display name, for the inspector. Defaults to the file stem.
    std::string name;

    std::array<PhaseDuration, WEATHER_PHASE_COUNT> phases{
        PhaseDuration{60.0f, 120.0f, false},    // approach
        PhaseDuration{20.0f, 40.0f, false},     // onset
        PhaseDuration{300.0f, 600.0f, false},   // sustain
        PhaseDuration{60.0f, 120.0f, false},    // tail
    };

    PrecipitationType precipitation_type{PrecipitationType::None};
    /// How wet exposed surfaces get while it rains, how covered while it
    /// snows, 0..1. Integrated over time, not blended.
    float wetness_target{1.0f};
    float snow_target{1.0f};

    /// The heading the wind blows toward, in degrees about the up axis, 0
    /// being +Z; `wind_veer` adds to it over the event. The front comes
    /// from upwind of that heading.
    float wind_heading{0.0f};

    /// What a strike of this event's lightning looks like. The rate comes
    /// from the `lightning_rate` track, so a storm can build and pass; this
    /// is what each strike is when one happens.
    struct Lightning {
        /// The flash over its own life, 0..1 in and 0..1 out. A strike is a
        /// stutter rather than a fade: two or three quick pulses is what
        /// tells lightning apart from a light being switched on, and that
        /// shape is authored here rather than fixed in the engine. Empty
        /// falls back to a single pulse.
        ftk::Curve flash;
        /// How long one strike's flash lasts, in seconds.
        float duration{0.35f};
        /// The colour it throws, linear. A cold front and a dust storm do
        /// not flash the same colour.
        glm::vec3 color{0.75f, 0.82f, 1.0f};
        /// How bright the light at the strike is, and how far it reaches.
        float intensity{2000.0f};
        float radius{400.0f};
        /// How far from the listener strikes land, in metres.
        float min_distance{200.0f};
        float max_distance{3000.0f};
        /// A bolt effect spawned at the strike, optional.
        std::string vfx;
    } lightning;

    /// Optional: the event brings its own sky and day profile, faded in by
    /// the `sky_blend` track. Empty keeps the scene's.
    std::string sky_shader;
    std::string day_profile;

    struct Vfx {
        std::string rain;
        std::string snow;
        std::string splash;
    } vfx;

    struct Audio {
        std::string bed;
        std::string wind;
        std::string thunder;
    } audio;

    /// Keyed in phase units 0..4, see above.
    TrackSet tracks;

    WeatherAsset() {
        tracks.length = static_cast<float>(WEATHER_PHASE_COUNT);
        tracks.wrap = false;
    }
};

[[nodiscard]] const char* weather_phase_name(WeatherPhase phase);

/// The phase a name spells, for a scene file or a command line.
/// Returns nothing for a name no phase answers to, so a typo reads as a
/// typo rather than silently becoming the approach.
[[nodiscard]] std::optional<WeatherPhase> weather_phase_from_name(std::string_view name);
[[nodiscard]] ftk::Result<WeatherAsset> load_weather_asset_file(const std::string& path);

/// A weather event read back from its text, as weather_asset_text() writes it. `path` names it in errors and gives the default name.
[[nodiscard]] ftk::Result<WeatherAsset> weather_asset_from_text(std::string_view text, const std::string& path);
[[nodiscard]] ftk::Result<> save_weather_asset_file(const std::string& path, const WeatherAsset& asset);

/// The .fjweather text save_weather_asset_file() writes, without writing it.
[[nodiscard]] std::string weather_asset_text(const WeatherAsset& asset);

} // namespace fjell
