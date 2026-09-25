#pragma once

#include "core/subsystem.hpp"
#include "environment/environment_state.hpp"
#include "environment/climate_director.hpp"
#include "environment/weather_player.hpp"
#include "renderer/resources/resource_handle.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fjell {

struct VfxEffect;

class AudioSubsystem;
class ScriptSubsystem;
class VfxSubsystem;
class ResourceManager;
class ThreadPool;
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

    /// The wind the simulation pushes with in the current fixed step: the
    /// frame's weather, its gust taken at the step's own time so the push is
    /// the same whatever the frame rate. What is drawn blows with
    /// `state().weather` instead.
    [[nodiscard]] glm::vec3 fixed_wind() const { return fixed_wind_; }

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
    /// What the simulation reads from the environment, advanced by the step:
    /// the wind it pushes with, the sea's lag behind the wind, the wave field
    /// that makes, and the wakes laid in it. What is only drawn stays in
    /// tick(). Lives in environment_fixed.cpp.
    void fixed_tick(const FixedTickContext& ctx) override;
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
    /// Keep what each body of water is made of, for the fog pass.
    void update_water_media(const TickContext& ctx);
    /// Blow the resolved wind through every body of water, so each one's
    /// effective wave field is the sea the weather is making of it, and lay
    /// the wakes of what moves through it: advanced `dt` to `time`, with the
    /// movers where `sources` puts them. The simulation does it once per step
    /// while playing; outside play the frame does, so the editor's sea still
    /// answers the weather.
    void advance_water(Scene& scene, std::span<const MotionSource> sources, float dt, float time,
                       ThreadPool* workers);
    /// Fire the script hooks for whatever changed since last frame.
    void dispatch_changes(const TickContext& ctx);
    void stop_beds(Beds& beds);
    void follow_event(Beds& beds, const WeatherPlayer::Playing* playing, const TickContext& ctx);

    // Keep the node's day profile pointer following its path.
    static void bind_day_profile(WorldEnvironmentNode& env, ResourceManager* resources);
    /// Keep the fog node's, every fog volume's and every post process
    /// volume's material following its path, and read each material's
    /// uniform values back onto its node so the resolver can seed the frame
    /// from plain data.
    static void bind_node_materials(Scene& scene, ResourceManager* resources,
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

    /// The simulation's side, refreshed each fixed step: the wind it pushes
    /// with, what stirs the water and where each of those was last step, to
    /// tell how fast a mover without a stated velocity goes.
    glm::vec3 fixed_wind_{0.0f};
    std::vector<MotionSource> fixed_sources_;
    std::unordered_map<uint32_t, glm::vec3> fixed_source_positions_;

    /// Breath, from whoever has their head under a water surface: every few
    /// seconds a cluster of larger bubbles and a short stream of small ones
    /// from the mouth, each given the life it needs to reach the surface. One
    /// emitter node per submerged head, kept for a bubble's life after the
    /// head surfaces so the last breath finishes rising.
    struct Breathing {
        uint32_t source_id{0};
        uint32_t node_id{0};
        std::shared_ptr<VfxEffect> effect;
        /// Seconds into the current breath cycle; a breath is let out at 0.
        float breath_clock{0.0f};
        /// Seconds since the head was last under the water.
        float surfaced_for{0.0f};
    };
    std::vector<Breathing> breathing_;
    void update_breathing(const TickContext& ctx);
    void stop_breathing(Scene& scene);

    /// Seconds between breaths, and how long the exhale lasts within one.
    static constexpr float BREATH_PERIOD = 3.4f;
    static constexpr float BREATH_EXHALE = 0.7f;
    /// The stream of small bubbles at the height of the exhale, per second.
    static constexpr float BREATH_STREAM_RATE = 45.0f;
    /// Where the mouth is, below the top of the motion source.
    static constexpr float BREATH_MOUTH_BELOW_TOP = 0.15f;
    /// How far out from the mouth a breath appears, as a share of the
    /// source's radius plus a margin: outside the head, where it can be seen,
    /// rather than inside the mesh until it has risen clear of it.
    static constexpr float BREATH_SPREAD_SHARE = 0.45f;
    static constexpr float BREATH_SPREAD_MARGIN = 0.06f;
    static constexpr std::string_view BREATH_EFFECT = "engine_assets/effects/breath.fjvfx";

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
