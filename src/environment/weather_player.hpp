#pragma once

#include "environment/weather_asset.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace fjell {

struct DayProfileAsset;
struct EnvironmentState;

/// How fast surfaces take on and shed the weather, in seconds to move most
/// of the way to the target. Project settings; the same for every event.
struct WeatherRates {
    float wetting_seconds{60.0f};
    float drying_seconds{600.0f};
    float snowing_seconds{300.0f};
    float melting_seconds{1200.0f};
};

/// A day profile and sky an event brings with it, and how far it has faded
/// in. The resolver blends the scene's profile toward it before the player
/// composites the rest.
struct WeatherSkyOverride {
    const DayProfileAsset* profile{nullptr};
    std::string sky_shader;
    float weight{0.0f};
};

/// Plays weather events: advances their phases, and composites the active
/// event and the one it is replacing into the resolved environment. The
/// outgoing event plays its tail while the incoming plays its approach, so a
/// handoff is a crossfade shaped by each event's own tracks rather than a
/// lerp of two forms. Wetness and snow integrate over time toward the
/// active event's targets so a street stays dark after the rain stops.
class WeatherPlayer {
public:
    struct Playing {
        const WeatherAsset* asset{nullptr};
        uint32_t event_id{0};
        const DayProfileAsset* profile{nullptr};   // the asset's day_profile, when bound
        WeatherPhase phase{WeatherPhase::Approach};
        float phase_time{0.0f};                      // seconds into the phase
        std::array<float, WEATHER_PHASE_COUNT> durations{};  // rolled, seconds; 0 = indefinite
        bool finished{false};

        /// Position on the event's track domain: phase index plus progress.
        [[nodiscard]] float phase_units() const;
        /// 0..1 through the current phase; an indefinite phase settles at 1.
        [[nodiscard]] float progress() const;
        /// How much of the event is present: rises over approach and onset,
        /// full through sustain, falls over the tail.
        [[nodiscard]] float weight() const;
    };

    /// Start an event. The event playing so far, if any, moves to its tail
    /// and plays out underneath. `seed` decides the rolled durations, so a
    /// director that replays a seed gets the same weather.
    void play(const WeatherAsset* asset, uint32_t event_id, uint32_t seed,
              const DayProfileAsset* profile = nullptr);
    /// Move the active event to its tail with nothing to follow.
    void stop();

    void tick(float dt);

    /// The sky and profile the active event brings, weighted by its presence
    /// and its sky_blend track; nothing when it brings none.
    [[nodiscard]] WeatherSkyOverride sky_override() const;

    /// Fold the playing events into `state` and advance the integrated
    /// wetness and snow by `dt`.
    void composite(EnvironmentState& state, float dt, const WeatherRates& rates);

    [[nodiscard]] const Playing* active() const { return has_active_ ? &active_ : nullptr; }
    [[nodiscard]] const Playing* outgoing() const { return has_outgoing_ ? &outgoing_ : nullptr; }
    [[nodiscard]] float wetness() const { return wetness_; }
    [[nodiscard]] float snow() const { return snow_; }

private:
    static void advance(Playing& p, float dt);
    static void begin_tail(Playing& p);

    Playing active_{};
    Playing outgoing_{};
    bool has_active_{false};
    bool has_outgoing_{false};
    float wetness_{0.0f};
    float snow_{0.0f};
    float gust_time_{0.0f};
    glm::vec2 wind_drift_{0.0f};
};

} // namespace fjell
