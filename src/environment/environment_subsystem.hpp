#pragma once

#include "core/subsystem.hpp"
#include "environment/environment_state.hpp"
#include "environment/climate_director.hpp"
#include "environment/weather_player.hpp"
#include "renderer/resources/resource_handle.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fjell {

class AudioSubsystem;
class ScriptSubsystem;
class VfxSubsystem;
class ResourceManager;
class Scene;
struct DayProfileAsset;
struct WeatherAsset;
struct WorldEnvironmentNode;
struct VolumetricFogNode;
class Project;

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
    /// Start an event, optionally partway through: `from` skips the phases
    /// before it, for an editor that wants the weather now.
    void play_weather(WeatherAssetHandle event, WeatherPhase from = WeatherPhase::Approach);
    [[nodiscard]] const WeatherPlayer& weather() const { return player_; }

    /// Freeze the climate: whatever plays keeps playing and nothing new is
    /// rolled until it is let go.
    /// Hand the weather to a climate, or to nothing with an invalid handle.
    void set_climate(ClimateAssetHandle climate);
    /// Strike lightning now: the playing event's, somewhere around the
    /// listener, or at `relative_position` when one is given.
    void strike_lightning(const glm::vec3& relative_position = glm::vec3(0.0f, 0.0f, 250.0f),
                          float intensity = 1.0f);
    void hold_weather(bool held) { director_.hold(held); }
    /// Fix what every weather roll derives from, so two runs of the same
    /// scene roll the same phase durations. For a measured run; zero, the
    /// default, lets each play roll freshly.
    void set_seed_base(uint32_t seed) { seed_base_ = seed; }
    /// Put the wetness and snow integrators where they would have got to.
    /// See WeatherPlayer::set_wetness.
    void set_wetness(float wetness) { player_.set_wetness(wetness); }
    void set_snow(float snow) { player_.set_snow(snow); }
    [[nodiscard]] bool weather_held() const { return director_.held(); }
    [[nodiscard]] const ClimateDirector& director() const { return director_; }
    /// The `.fjweather` path the climate's `index`-th forecast roll plays,
    /// empty past the end of the forecast.
    [[nodiscard]] std::string forecast_event(uint32_t index) const;

    /// A sky shader uniform value set from a script, laid over the node's
    /// overrides and the profile's tracks until cleared by an empty count.
    void set_sky_uniform(const NamedValue& value);

    /// Where the weather's rain and wind beds play. Optional; without it the
    /// weather is silent.
    void set_audio(AudioSubsystem* audio) { audio_ = audio; }

    /// Where the weather and time-phase hooks are dispatched. Optional;
    /// without it the changes still happen, no script hears about them.
    void set_scripts(ScriptSubsystem* scripts) { scripts_ = scripts; }

    /// Where a strike's bolt effect is spawned. Optional.
    void set_vfx(VfxSubsystem* vfx) { vfx_ = vfx; }

protected:
    void on_play_stop(const TickContext& ctx) override;

private:
    // The looping sounds a playing event brings, and the event they belong to.
    struct Beds {
        uint32_t event_id{0};
        const WeatherAsset* asset{nullptr};
        uint32_t bed{0};
        uint32_t wind{0};
    };
    void update_beds(const TickContext& ctx);
    /// Keep a hidden emitter per falling effect parked at the camera, its
    /// spawn rate following how hard it falls. Both events' effects run
    /// during a handoff, each at its own weight, so rain fades into snow.
    /// Carry each fog volume's noise field along by one frame's drift.
    void advance_fog_volumes(const TickContext& ctx);
    void update_precipitation(const TickContext& ctx);
    void stop_precipitation(Scene& scene);
    /// Light the world from a strike and set its thunder on its way.
    void update_lightning(const TickContext& ctx);
    void stop_lightning(Scene& scene);
    /// Blow the resolved wind through every body of water, so each one's
    /// effective wave field is the sea the weather is making of it.
    void update_water(const TickContext& ctx);
    /// Fire the script hooks for whatever changed since last frame.
    void dispatch_changes(const TickContext& ctx);
    void stop_beds(Beds& beds);
    void follow_event(Beds& beds, const WeatherPlayer::Playing* playing, const TickContext& ctx);

    // Keep the node's day profile pointer following its path.
    static void bind_day_profile(WorldEnvironmentNode& env, ResourceManager* resources);
    /// Keep the fog node's and every fog volume's material following its
    /// path, and read each material's uniform values back onto its node so
    /// the resolver can seed the frame from plain data.
    static void bind_fog_materials(Scene& scene, ResourceManager* resources,
                                   const Project* project);
    // Keep the node's weather pointer following its path; a new path plays.
    void bind_weather(WorldEnvironmentNode& env, ResourceManager* resources);
    void bind_climate(WorldEnvironmentNode& env, ResourceManager* resources);
    /// Start whatever the director's current roll names.
    void play_directed(ResourceManager* resources);
    [[nodiscard]] const DayProfileAsset* profile_for(const WeatherAsset& event) const;

    Scene* scene_{nullptr};
    ResourceManager* resources_{nullptr};
    EnvironmentState state_;
    // Which body cast the shadows last frame, so the handoff has hysteresis.
    bool moon_led_{false};

    /// The wind the sea has caught up to, which lags the wind that is
    /// blowing. Water has mass: a front does not raise a swell the instant
    /// its wind arrives, and dropping the wind does not flatten one. Unset
    /// until the first tick, which starts it at whatever is already blowing
    /// rather than building the sea up from a calm the scene never had.
    std::optional<glm::vec2> sea_wind_;

    WeatherPlayer player_;
    ClimateDirector director_;
    WeatherRates rates_;
    AudioSubsystem* audio_{nullptr};
    ScriptSubsystem* scripts_{nullptr};
    VfxSubsystem* vfx_{nullptr};
    // What the last frame reported, so a change is what fires a hook.
    uint32_t last_event_{0};
    WeatherPhase last_weather_phase_{WeatherPhase::Sustain};
    TimePhase last_time_phase_{TimePhase::Day};
    bool phases_seen_{false};
    Beds active_beds_;
    Beds outgoing_beds_;

    // The node carrying one event's falling effect, and which event it is.
    struct Precipitation {
        uint32_t event_id{0};
        uint32_t node_id{0};
        std::string path;
    };
    Precipitation active_fall_;
    Precipitation outgoing_fall_;

    // A strike's light, alive for the flash's length, and the thunder still
    // travelling toward the listener.
    struct StrikeLight {
        uint32_t node_id{0};
        float remaining{0.0f};
        float duration{1.0f};
        float intensity{0.0f};
    };
    std::vector<StrikeLight> strike_lights_;
    struct PendingThunder {
        float delay{0.0f};
        float volume{1.0f};
        glm::vec3 position{0.0f};
        std::string clip;
    };
    std::vector<PendingThunder> pending_thunder_;
    uint32_t play_count_{0};   // seeds the rolled durations
    // Mixed into every roll's seed. Zero lets the play count alone decide,
    // which is what an ordinary session wants: replaying an event gives it
    // fresh durations. A capture sets it so two runs roll identically.
    uint32_t seed_base_{0};
    std::vector<NamedValue> script_sky_uniforms_;
};

} // namespace fjell
