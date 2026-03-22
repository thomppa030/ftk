#include "animation/animation_state_machine.hpp"
#include "core/log.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>

namespace fjell {

// ── JSON loading ────────────────────────────────────────────────────────

static CompareOp parse_op(const std::string& s) {
    if (s == "==" || s == "eq")  return CompareOp::equal;
    if (s == "!=" || s == "ne")  return CompareOp::not_equal;
    if (s == "<"  || s == "lt")  return CompareOp::less;
    if (s == "<=" || s == "le")  return CompareOp::less_equal;
    if (s == ">"  || s == "gt")  return CompareOp::greater;
    if (s == ">=" || s == "ge")  return CompareOp::greater_equal;
    FJELL_CORE_WARN("Unknown compare op '{}', defaulting to ==", s);
    return CompareOp::equal;
}

void AnimationStateMachine::load(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        throw std::runtime_error("Failed to open .fjanimset: " + path);
    }

    nlohmann::json j;
    f >> j;

    states_.clear();
    state_index_.clear();
    params_.clear();
    current_.clear();
    next_.clear();
    crossfade_alpha_ = 0.0f;

    // Parse states
    if (!j.contains("states") || !j["states"].is_array()) {
        throw std::runtime_error(".fjanimset missing 'states' array: " + path);
    }

    for (const auto& js : j["states"]) {
        AnimationState state;
        state.name = js.at("name").get<std::string>();
        state.clip = js.at("clip").get<std::string>();
        state.speed = js.value("speed", 1.0f);
        state.looping = js.value("loop", true);

        if (js.contains("transitions")) {
            for (const auto& jt : js["transitions"]) {
                StateTransition transition;
                transition.target = jt.at("target").get<std::string>();
                transition.blend_time = jt.value("blend_time", 0.2f);

                if (jt.contains("conditions")) {
                    for (const auto& jc : jt["conditions"]) {
                        TransitionCondition cond;
                        cond.param = jc.at("param").get<std::string>();
                        cond.op = parse_op(jc.value("op", "=="));
                        cond.value = jc.at("value").get<float>();
                        transition.conditions.push_back(std::move(cond));
                    }
                }

                state.transitions.push_back(std::move(transition));
            }
        }

        state_index_[state.name] = static_cast<uint32_t>(states_.size());
        states_.push_back(std::move(state));
    }

    // Default parameters
    if (j.contains("params")) {
        for (auto& [name, val] : j["params"].items()) {
            params_[name] = val.get<float>();
        }
    }

    // Initial state
    current_ = j.value("initial", states_.empty() ? "" : states_[0].name);
}

// ── Runtime ─────────────────────────────────────────────────────────────

bool AnimationStateMachine::update(float dt) {
    // Advance crossfade if transitioning
    if (!next_.empty()) {
        if (crossfade_duration_ > 0.0f) {
            crossfade_alpha_ += dt / crossfade_duration_;
        } else {
            crossfade_alpha_ = 1.0f;
        }

        if (crossfade_alpha_ >= 1.0f) {
            // Transition complete
            current_ = next_;
            next_.clear();
            crossfade_alpha_ = 0.0f;
            crossfade_duration_ = 0.0f;
        }
        return false;
    }

    // Evaluate outgoing transitions from current state
    const auto* state = current_state();
    if (!state) return false;

    for (const auto& t : state->transitions) {
        if (evaluate_conditions(t.conditions)) {
            next_ = t.target;
            crossfade_duration_ = t.blend_time;
            crossfade_alpha_ = 0.0f;
            on_transition.broadcast(current_, next_);
            return true;
        }
    }

    return false;
}

void AnimationStateMachine::set_param(const std::string& name, float value) {
    params_[name] = value;
}

float AnimationStateMachine::get_param(const std::string& name) const {
    auto it = params_.find(name);
    return it != params_.end() ? it->second : 0.0f;
}

const AnimationState* AnimationStateMachine::current_state() const {
    auto it = state_index_.find(current_);
    return it != state_index_.end() ? &states_[it->second] : nullptr;
}

const AnimationState* AnimationStateMachine::next_state() const {
    if (next_.empty()) return nullptr;
    auto it = state_index_.find(next_);
    return it != state_index_.end() ? &states_[it->second] : nullptr;
}

const AnimationState* AnimationStateMachine::find_state(const std::string& name) const {
    auto it = state_index_.find(name);
    return it != state_index_.end() ? &states_[it->second] : nullptr;
}

bool AnimationStateMachine::evaluate_conditions(const std::vector<TransitionCondition>& conditions) const {
    for (const auto& c : conditions) {
        float param_val = get_param(c.param);
        bool pass = false;
        switch (c.op) {
            case CompareOp::equal:         pass = (param_val == c.value); break;
            case CompareOp::not_equal:     pass = (param_val != c.value); break;
            case CompareOp::less:          pass = (param_val <  c.value); break;
            case CompareOp::less_equal:    pass = (param_val <= c.value); break;
            case CompareOp::greater:       pass = (param_val >  c.value); break;
            case CompareOp::greater_equal: pass = (param_val >= c.value); break;
        }
        if (!pass) return false;
    }
    return true;
}

} // namespace fjell
