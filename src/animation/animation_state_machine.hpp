#pragma once

#include "core/delegate.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace fjell {

enum class CompareOp : uint8_t {
    equal,
    not_equal,
    less,
    less_equal,
    greater,
    greater_equal,
};

struct TransitionCondition {
    std::string param;
    CompareOp op{CompareOp::equal};
    float value{0.0f};
};

struct StateTransition {
    std::string target;
    float blend_time{0.2f};
    std::vector<TransitionCondition> conditions;
};

// A single state in the animation state machine.
// Can reference a single clip OR a blend space (1D or 2D).
struct AnimationState {
    std::string name;
    std::string clip;           // path to .fjanim (single clip mode)
    std::string blend_space;    // path to .fjblend1D or .fjblend2D (blend space mode)
    float speed{1.0f};
    bool looping{true};
    std::vector<StateTransition> transitions;

    [[nodiscard]] bool uses_blend_space() const { return !blend_space.empty(); }
};

// Data-driven animation state machine loaded from .fjanimset JSON.
// Evaluates transitions each frame based on float parameters.
class AnimationStateMachine {
public:
    // Load state machine definition from a .fjanimset JSON file.
    void load(const std::string& path);

    // Evaluate transitions and advance crossfade. Call once per frame.
    // Returns true if a transition occurred this frame.
    bool update(float dt);

    // Parameter access (used by gameplay code to drive transitions).
    void set_param(const std::string& name, float value);
    [[nodiscard]] float get_param(const std::string& name) const;

    // Current playback state.
    [[nodiscard]] const std::string& current_state_name() const { return current_; }
    [[nodiscard]] const AnimationState* current_state() const;
    [[nodiscard]] const AnimationState* next_state() const;
    [[nodiscard]] float crossfade_alpha() const { return crossfade_alpha_; }
    [[nodiscard]] bool is_transitioning() const { return !next_.empty(); }

    [[nodiscard]] const std::vector<AnimationState>& states() const { return states_; }
    [[nodiscard]] const AnimationState* find_state(const std::string& name) const;

    // Fired when a transition begins (from_state, to_state).
    Delegate<void(const std::string&, const std::string&)> on_transition;

private:
    [[nodiscard]] bool evaluate_conditions(const std::vector<TransitionCondition>& conditions) const;

    std::vector<AnimationState> states_;
    std::unordered_map<std::string, uint32_t> state_index_;
    std::unordered_map<std::string, float> params_;

    std::string current_;       // current state name
    std::string next_;          // target state during crossfade (empty = no transition)
    float crossfade_alpha_{0.0f};
    float crossfade_duration_{0.0f};
};

} // namespace fjell
