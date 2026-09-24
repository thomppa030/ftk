#pragma once

#include "fjell/api.h"
#include "core/delegate.hpp"
#include "core/result.hpp"

#include <cstdint>
#include <string>
#include <string_view>
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
    // Load/save state machine definition from/to a .fjanimset JSON file.
    [[nodiscard]] Result<> load(const std::string& path);
    /// Replaces the definition with one read back from text().
    [[nodiscard]] Result<> load_text(std::string_view text, const std::string& path);
    [[nodiscard]] Result<> save(const std::string& path) const;
    /// The .fjanimset text save() writes, without writing it.
    [[nodiscard]] std::string text() const;

    // Evaluate transitions and advance crossfade. Call once per frame.
    // Returns true if a transition occurred this frame.
    bool update(float dt);

    // Parameter access (used by gameplay code to drive transitions).
    FJELL_SDK_EXPORT void set_param(const std::string& name, float value);
    [[nodiscard]] float get_param(const std::string& name) const;

    // Current playback state.
    [[nodiscard]] const std::string& current_state_name() const { return current_; }
    [[nodiscard]] const AnimationState* current_state() const;
    [[nodiscard]] const AnimationState* next_state() const;
    [[nodiscard]] float crossfade_alpha() const { return crossfade_alpha_; }
    [[nodiscard]] bool is_transitioning() const { return !next_.empty(); }

    [[nodiscard]] const std::vector<AnimationState>& states() const { return states_; }
    [[nodiscard]] const AnimationState* find_state(const std::string& name) const;

    // Skeleton path stored in .fjanimset (relative asset path).
    [[nodiscard]] const std::string& skeleton_path() const { return skeleton_path_; }
    void set_skeleton_path(const std::string& path) { skeleton_path_ = path; }

    // Mutable access for editor.
    [[nodiscard]] std::vector<AnimationState>& states_mutable() { return states_; }
    [[nodiscard]] std::unordered_map<std::string, float>& params_mutable() { return params_; }
    void set_initial(const std::string& name) { current_ = name; }
    [[nodiscard]] const std::string& initial_state_name() const { return current_; }

    // Copy states, params, skeleton path, and current state from another state machine.
    // Does not copy the on_transition delegate or crossfade state.
    void copy_definition_from(const AnimationStateMachine& other);

    // Add/remove states (rebuilds state_index_).
    void add_state(AnimationState state);
    void remove_state(const std::string& name);
    void rebuild_index();

    // Fired when a transition begins (from_state, to_state).
    Delegate<void(const std::string&, const std::string&)> on_transition;

private:
    [[nodiscard]] bool evaluate_conditions(const std::vector<TransitionCondition>& conditions) const;

    std::vector<AnimationState> states_;
    std::unordered_map<std::string, uint32_t> state_index_;
    std::unordered_map<std::string, float> params_;
    std::string skeleton_path_; // relative asset path to .fjskel

    std::string current_;       // current state name
    std::string next_;          // target state during crossfade (empty = no transition)
    float crossfade_alpha_{0.0f};
    float crossfade_duration_{0.0f};
};

} // namespace fjell
