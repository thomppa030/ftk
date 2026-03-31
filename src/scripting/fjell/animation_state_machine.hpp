// animation_state_machine.hpp — Animation state machine for game scripts.
// Copy of animation/animation_state_machine.hpp, shipped as part of the SDK.
#pragma once

#include "fjell/delegate.hpp"

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

struct AnimationState {
    std::string name;
    std::string clip;           // path to .fjanim (single clip mode)
    std::string blend_space;    // path to .fjblend1D or .fjblend2D (blend space mode)
    float speed{1.0f};
    bool looping{true};
    std::vector<StateTransition> transitions;

    [[nodiscard]] bool uses_blend_space() const { return !blend_space.empty(); }
};

class AnimationStateMachine {
public:
    void load(const std::string& path);
    bool update(float dt);

    void set_param(const std::string& name, float value);
    [[nodiscard]] float get_param(const std::string& name) const;

    [[nodiscard]] const std::string& current_state_name() const { return current_; }
    [[nodiscard]] const AnimationState* current_state() const;
    [[nodiscard]] const AnimationState* next_state() const;
    [[nodiscard]] float crossfade_alpha() const { return crossfade_alpha_; }
    [[nodiscard]] bool is_transitioning() const { return !next_.empty(); }

    [[nodiscard]] const std::vector<AnimationState>& states() const { return states_; }
    [[nodiscard]] const AnimationState* find_state(const std::string& name) const;

    Delegate<void(const std::string&, const std::string&)> on_transition;

private:
    [[nodiscard]] bool evaluate_conditions(const std::vector<TransitionCondition>& conditions) const;

    std::vector<AnimationState> states_;
    std::unordered_map<std::string, uint32_t> state_index_;
    std::unordered_map<std::string, float> params_;

    std::string current_;
    std::string next_;
    float crossfade_alpha_{0.0f};
    float crossfade_duration_{0.0f};
};

} // namespace fjell
