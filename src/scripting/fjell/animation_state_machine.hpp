// animation_state_machine.hpp — Animation state machine for game scripts.
// Copy of animation/animation_state_machine.hpp, shipped as part of the SDK.
#pragma once

#include "fjell/api.h"
#include "fjell/delegate.hpp"

#include <glm/vec2.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fjell {

/// How a transition condition compares a parameter against its value.
enum class CompareOp : uint8_t {
    equal,
    not_equal,
    less,
    less_equal,
    greater,
    greater_equal,
};

/// One test against a state machine parameter. A transition fires only when
/// all of its conditions hold.
struct TransitionCondition {
    /// Parameter name, as set through `AnimationStateMachine::set_param()`.
    std::string param;

    /// Comparison to apply.
    CompareOp op{CompareOp::equal};

    /// Value to compare against. Booleans are 0.0 / 1.0.
    float value{0.0f};
};

/// An edge from one animation state to another.
struct StateTransition {
    /// Name of the state to move to.
    std::string target;

    /// Seconds spent crossfading into the target. 0 snaps instantly.
    float blend_time{0.2f};

    /// Conditions that must all hold for this transition to fire. An empty
    /// list fires as soon as it is evaluated.
    std::vector<TransitionCondition> conditions;
};

/// One state in an animation state machine — a clip or blend space to play,
/// plus the transitions leaving it.
struct AnimationState {
    /// State name, as `AnimationStateMachine::current_state_name()` reports it.
    std::string name;

    /// Path to a single `.fjanim` clip. Empty when using a blend space.
    std::string clip;

    /// Path to a `.fjblend1D` or `.fjblend2D`, for blending several clips by
    /// parameter (a walk/run blend by speed). Empty when using a single clip.
    std::string blend_space;

    /// Playback rate multiplier. 1.0 is the authored speed.
    float speed{1.0f};

    /// Whether playback repeats. A non-looping state sets
    /// `SpriteAnimComponent::finished` when it ends.
    bool looping{true};

    /// Transitions leaving this state, evaluated in order — the first whose
    /// conditions hold wins.
    std::vector<StateTransition> transitions;

    /// Where the editor's state graph draws this state. Kept so the layout
    /// survives a save; nothing at runtime reads it.
    std::optional<glm::vec2> editor_position;

    /// Whether this state blends multiple clips rather than playing one.
    [[nodiscard]] bool uses_blend_space() const { return !blend_space.empty(); }
};

/// Runs an animation graph authored in the editor: parameters go in,
/// transitions fire, and the current state says what to play.
///
/// A sprite animation's machine is `SpriteAnimComponent::state_machine`, a
/// skeletal one's `AnimatorComponent::state_machine()`: scripts set
/// parameters on either directly.
class AnimationStateMachine {
public:
    /// Load a state machine asset, replacing the current graph.
    /// @param path Path to the state machine file.
    void load(const std::string& path);

    /// Advance the machine and evaluate transitions. The engine calls this;
    /// game code should not.
    /// @param dt Seconds since the previous frame.
    /// @return True if the state changed this frame.
    bool update(float dt);

    /// Set a parameter, potentially firing a transition on the next update.
    /// @param name  Parameter name. Unknown names are created, so typos show
    ///              up as transitions that never fire.
    /// @param value Booleans are 0.0 / 1.0.
    FJELL_SDK_EXPORT void set_param(const std::string& name, float value);

    /// Read a parameter.
    /// @return Its value, or 0.0 if never set.
    [[nodiscard]] FJELL_SDK_EXPORT float get_param(const std::string& name) const;

    /// Name of the state currently playing. During a crossfade this stays the
    /// state being blended *from* until the transition completes.
    [[nodiscard]] const std::string& current_state_name() const { return current_; }

    /// The state currently playing, or null before any state is entered.
    [[nodiscard]] FJELL_SDK_EXPORT const AnimationState* current_state() const;

    /// The state being blended toward, or null when not transitioning.
    [[nodiscard]] FJELL_SDK_EXPORT const AnimationState* next_state() const;

    /// Crossfade progress, 0 at the start of a transition to 1 at its end.
    /// Meaningless unless `is_transitioning()`.
    [[nodiscard]] float crossfade_alpha() const { return crossfade_alpha_; }

    /// Whether a crossfade is in progress.
    [[nodiscard]] bool is_transitioning() const { return !next_.empty(); }

    /// Every state in the loaded graph.
    [[nodiscard]] const std::vector<AnimationState>& states() const { return states_; }

    /// Look up a state by name.
    /// @return The state, or null if the graph has no such state.
    [[nodiscard]] FJELL_SDK_EXPORT const AnimationState* find_state(const std::string& name) const;

    /// Fires when a transition begins, with `(from_state, to_state)`. Store
    /// the returned `Connection` — dropping it unsubscribes.
    /// @code
    /// conn_ = sm.on_transition.bind([](const std::string& from,
    ///                                  const std::string& to) { /* ... */ });
    /// @endcode
    Delegate<void(const std::string&, const std::string&)> on_transition;

private:
    [[nodiscard]] bool evaluate_conditions(const std::vector<TransitionCondition>& conditions) const;

    std::vector<AnimationState> states_;
    std::unordered_map<std::string, uint32_t> state_index_;
    std::unordered_map<std::string, float> params_;
    std::string skeleton_path_;

    std::string current_;
    std::string next_;
    float crossfade_alpha_{0.0f};
    float crossfade_duration_{0.0f};
};

} // namespace fjell
