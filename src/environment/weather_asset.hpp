#pragma once

#include "core/result.hpp"
#include "environment/track_set.hpp"

#include <array>
#include <cstdint>
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

enum class WeatherPhase : uint8_t { Approach = 0, Onset = 1, Sustain = 2, Tail = 3 };
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
[[nodiscard]] Result<WeatherAsset> load_weather_asset_file(const std::string& path);
[[nodiscard]] Result<> save_weather_asset_file(const std::string& path, const WeatherAsset& asset);

} // namespace fjell
