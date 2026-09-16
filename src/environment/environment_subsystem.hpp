#pragma once

#include "core/subsystem.hpp"
#include "environment/environment_state.hpp"

#include <string_view>

namespace fjell {

class ResourceManager;
class Scene;
struct WorldEnvironmentNode;

/// Owns the resolved environment of the scene the engine ticks, and the
/// clock behind its time of day. Runs every frame, in and out of play, so the
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

private:
    // Keep the node's day profile pointer following its path.
    static void bind_day_profile(WorldEnvironmentNode& env, ResourceManager* resources);

    Scene* scene_{nullptr};
    EnvironmentState state_;
    // Which body cast the shadows last frame, so the handoff has hysteresis.
    bool moon_led_{false};
};

} // namespace fjell
