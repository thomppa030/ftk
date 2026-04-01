#pragma once

#include "core/input_action.hpp"
#include "core/input_mapping_context.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace fjell {

class Input;

class InputActionMap {
public:
    // Asset loading
    void load_action(const std::string& path);
    void load_context(const std::string& path);
    void load_actions_dir(const std::string& dir);

    // Evaluate all bindings against raw input, fire delegates
    void update(const Input& raw_input);

    // Context stack
    void push_context(const std::string& name);
    void pop_context(const std::string& name);

    // Polling queries
    [[nodiscard]] bool is_pressed(const std::string& action) const;
    [[nodiscard]] bool is_held(const std::string& action) const;
    [[nodiscard]] bool is_released(const std::string& action) const;
    [[nodiscard]] float axis1d(const std::string& action) const;
    [[nodiscard]] glm::vec2 axis2d(const std::string& action) const;

    // Delegate access (for wiring [[fj::action]] methods)
    [[nodiscard]] ActionDelegates* delegates_for(const std::string& action);

    // Rebinding
    void rebind(const std::string& context, const std::string& action,
                int binding_index, InputSource new_source);
    void save_context(const std::string& context, const std::string& path) const;

    // Listen mode for editor rebinding
    void start_listening();
    [[nodiscard]] bool is_listening() const { return listening_; }
    [[nodiscard]] InputSource last_captured() const { return captured_; }
    void cancel_listening();

    // Editor access
    [[nodiscard]] const auto& actions() const { return actions_; }
    [[nodiscard]] const auto& contexts() const { return contexts_; }
    [[nodiscard]] const auto& states() const { return states_; }
    [[nodiscard]] const auto& context_stack() const { return context_stack_; }

private:
    void evaluate_binding(const InputBinding& binding, const Input& raw_input,
                          std::unordered_map<std::string, bool>& consumed);

    float read_source_value(InputSource source, const Input& raw_input) const;
    bool read_source_held(InputSource source, const Input& raw_input) const;

    std::unordered_map<std::string, InputActionDef> actions_;
    std::unordered_map<std::string, InputMappingContext> contexts_;
    std::vector<std::string> context_stack_;  // top = back
    std::unordered_map<std::string, ActionState> states_;
    std::unordered_map<std::string, ActionDelegates> delegates_;

    bool listening_{false};
    InputSource captured_{InputSource::none};
};

} // namespace fjell
