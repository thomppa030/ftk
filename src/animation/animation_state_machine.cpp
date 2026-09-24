#include "animation/animation_state_machine.hpp"
#include "fjell/api.h"
#include "core/log.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>

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

Result<> AnimationStateMachine::load(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        return make_error("Failed to open .fjanimset: " + path);
    }
    std::stringstream text;
    text << f.rdbuf();
    return load_text(text.str(), path);
}

Result<> AnimationStateMachine::load_text(std::string_view text, const std::string& path) {
    auto j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object()) return make_error("Malformed .fjanimset: " + path);

    states_.clear();
    state_index_.clear();
    params_.clear();
    skeleton_path_.clear();
    current_.clear();
    next_.clear();
    crossfade_alpha_ = 0.0f;

    // Skeleton reference
    skeleton_path_ = j.value("skeleton", "");

    // Parse states
    if (!j.contains("states") || !j["states"].is_array()) {
        return make_error(".fjanimset missing 'states' array: " + path);
    }

    for (const auto& js : j["states"]) {
        AnimationState state;
        state.name = js.at("name").get<std::string>();
        state.clip = js.value("clip", "");
        state.blend_space = js.value("blend_space", "");
        state.speed = js.value("speed", 1.0f);
        state.looping = js.value("loop", true);
        if (const auto it = js.find("editor_position"); it != js.end() && it->is_array() && it->size() == 2) {
            state.editor_position = glm::vec2((*it)[0].get<float>(), (*it)[1].get<float>());
        }

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
    return {};
}

// ── JSON saving ────────────────────────────────────────────────────

static std::string format_op(CompareOp op) {
    switch (op) {
        case CompareOp::equal:         return "==";
        case CompareOp::not_equal:     return "!=";
        case CompareOp::less:          return "<";
        case CompareOp::less_equal:    return "<=";
        case CompareOp::greater:       return ">";
        case CompareOp::greater_equal: return ">=";
    }
    return "==";
}

std::string AnimationStateMachine::text() const {
    nlohmann::json j;

    if (!skeleton_path_.empty()) {
        j["skeleton"] = skeleton_path_;
    }
    j["initial"] = current_;

    if (!params_.empty()) {
        j["params"] = nlohmann::json::object();
        for (const auto& [name, val] : params_) {
            j["params"][name] = val;
        }
    }

    j["states"] = nlohmann::json::array();
    for (const auto& state : states_) {
        nlohmann::json js;
        js["name"] = state.name;
        if (!state.clip.empty()) js["clip"] = state.clip;
        if (!state.blend_space.empty()) js["blend_space"] = state.blend_space;
        if (state.speed != 1.0f) js["speed"] = state.speed;
        if (!state.looping) js["loop"] = state.looping;
        if (state.editor_position) js["editor_position"] = {state.editor_position->x, state.editor_position->y};

        if (!state.transitions.empty()) {
            js["transitions"] = nlohmann::json::array();
            for (const auto& t : state.transitions) {
                nlohmann::json jt;
                jt["target"] = t.target;
                if (t.blend_time != 0.2f) jt["blend_time"] = t.blend_time;

                if (!t.conditions.empty()) {
                    jt["conditions"] = nlohmann::json::array();
                    for (const auto& c : t.conditions) {
                        jt["conditions"].push_back({
                            {"param", c.param},
                            {"op", format_op(c.op)},
                            {"value", c.value}
                        });
                    }
                }

                js["transitions"].push_back(std::move(jt));
            }
        }

        j["states"].push_back(std::move(js));
    }

    return j.dump(2);
}

Result<> AnimationStateMachine::save(const std::string& path) const {
    std::ofstream file(path);
    if (!file) {
        return make_error("Failed to save .fjanimset: " + path);
    }
    file << text();
    return {};
}

void AnimationStateMachine::copy_definition_from(const AnimationStateMachine& other) {
    states_ = other.states_;
    params_ = other.params_;
    skeleton_path_ = other.skeleton_path_;
    current_ = other.current_;
    next_.clear();
    crossfade_alpha_ = 0.0f;
    crossfade_duration_ = 0.0f;
    rebuild_index();
}

// ── Mutation ───────────────────────────────────────────────────────

void AnimationStateMachine::add_state(AnimationState state) {
    state_index_[state.name] = static_cast<uint32_t>(states_.size());
    states_.push_back(std::move(state));
}

void AnimationStateMachine::remove_state(const std::string& name) {
    auto it = state_index_.find(name);
    if (it == state_index_.end()) return;

    states_.erase(states_.begin() + it->second);

    // Clean up transitions referencing the removed state
    for (auto& state : states_) {
        std::erase_if(state.transitions, [&name](const StateTransition& t) {
            return t.target == name;
        });
    }

    // Fix initial state if it was removed
    if (current_ == name) {
        current_ = states_.empty() ? "" : states_[0].name;
    }
    if (next_ == name) {
        next_.clear();
        crossfade_alpha_ = 0.0f;
    }

    rebuild_index();
}

void AnimationStateMachine::rebuild_index() {
    state_index_.clear();
    for (uint32_t i = 0; i < static_cast<uint32_t>(states_.size()); ++i) {
        state_index_[states_[i].name] = i;
    }
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

FJELL_SDK_EXPORT void AnimationStateMachine::set_param(const std::string& name, float value) {
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
