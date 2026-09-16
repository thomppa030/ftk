#pragma once

#include "core/subsystem.hpp"
#include "environment/environment_state.hpp"
#include "environment/weather_player.hpp"
#include "renderer/resources/resource_handle.hpp"

#include <string_view>
#include <vector>

namespace fjell {

class ResourceManager;
class Scene;
struct DayProfileAsset;
struct WeatherAsset;
struct WorldEnvironmentNode;

/// Owns the resolved environment of the scene the engine ticks, the clock
/// behind its time of day, and the weather playing over it. Runs every frame, in and out of play, so the
/// editor viewport shows the same lighting the game will: the renderer reads
/// `state()` and never the scene's environment nodes.
///
/// The hour advances while playing, or in the editor when the world
/// environment node asks for it, and the resolved sun direction is written
/// back to the scene's sun light so the inspector and gizmos show where the
/// light really comes from.
class EnvironmentSubsystem : public Subsystem {
public:
    [[nodiscard]] std::string_view name() const override { return "Environment"; }

    void tick(const TickContext& ctx) override;

    [[nodiscard]] const EnvironmentState& state() const { return state_; }

    // ── Time of day control ──
    // For scripts and the editor. Each writes the ticked scene's world
    // environment node; without one there is no clock and the calls do
    // nothing, reads report the resolved defaults.
    void set_hour(float hour);
    [[nodiscard]] float hour() const { return state_.hour; }
    void set_day_length(float seconds);
    [[nodiscard]] float day_length() const;

    // ── Weather control ──
    /// Start a weather event now; the one playing tails out underneath it.
    /// A null handle tails the current event out with nothing to follow.
    void play_weather(WeatherAssetHandle event);
    [[nodiscard]] const WeatherPlayer& weather() const { return player_; }

    /// A sky shader uniform value set from a script, laid over the node's
    /// overrides and the profile's tracks until cleared by an empty count.
    void set_sky_uniform(const NamedValue& value);

private:
    // Keep the node's day profile pointer following its path.
    static void bind_day_profile(WorldEnvironmentNode& env, ResourceManager* resources);
    // Keep the node's weather pointer following its path; a new path plays.
    void bind_weather(WorldEnvironmentNode& env, ResourceManager* resources);
    [[nodiscard]] const DayProfileAsset* profile_for(const WeatherAsset& event) const;

    Scene* scene_{nullptr};
    ResourceManager* resources_{nullptr};
    EnvironmentState state_;
    // Which body cast the shadows last frame, so the handoff has hysteresis.
    bool moon_led_{false};

    WeatherPlayer player_;
    WeatherRates rates_;
    uint32_t play_count_{0};   // seeds the rolled durations
    std::vector<NamedValue> script_sky_uniforms_;
};

} // namespace fjell
