#include "core/input_action_map.hpp"
#include "core/input.hpp"
#include "core/log.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>

namespace fjell {

// ── Asset loading ────────────────────────────────────────────────────

void InputActionMap::load_action(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        FJELL_CORE_WARN("InputActionMap: failed to open {}", path);
        return;
    }

    auto j = nlohmann::json::parse(f, nullptr, false);
    if (!j.is_object()) {
        FJELL_CORE_WARN("InputActionMap: invalid JSON in {}", path);
        return;
    }

    InputActionDef def;
    def.name = j.value("name", "");
    if (def.name.empty()) {
        FJELL_CORE_WARN("InputActionMap: action missing 'name' in {}", path);
        return;
    }

    auto type_str = j.value("type", "button");
    if (type_str == "button") def.type = ActionType::button;
    else if (type_str == "axis1d") def.type = ActionType::axis1d;
    else if (type_str == "axis2d") def.type = ActionType::axis2d;
    else {
        FJELL_CORE_WARN("InputActionMap: unknown type '{}' for action '{}'", type_str, def.name);
        def.type = ActionType::button;
    }

    actions_[def.name] = def;
    // Ensure state and delegates exist
    states_[def.name].type = def.type;
    delegates_[def.name];

    FJELL_CORE_INFO("InputActionMap: loaded action '{}' ({})", def.name, type_str);
}

void InputActionMap::load_actions_dir(const std::string& dir) {
    namespace fs = std::filesystem;
    if (!fs::exists(dir)) return;

    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".fjaction") {
            load_action(entry.path().string());
        }
    }
}

static ModifierType parse_modifier_type(const std::string& s) {
    if (s == "scale") return ModifierType::scale;
    if (s == "deadzone") return ModifierType::deadzone;
    if (s == "negate") return ModifierType::negate;
    if (s == "swizzle") return ModifierType::swizzle;
    return ModifierType::scale;
}

void InputActionMap::load_context(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        FJELL_CORE_WARN("InputActionMap: failed to open {}", path);
        return;
    }

    auto j = nlohmann::json::parse(f, nullptr, false);
    if (!j.is_object()) {
        FJELL_CORE_WARN("InputActionMap: invalid JSON in {}", path);
        return;
    }

    InputMappingContext ctx;
    ctx.name = j.value("name", "");
    ctx.consume = j.value("consume", true);

    if (ctx.name.empty()) {
        FJELL_CORE_WARN("InputActionMap: context missing 'name' in {}", path);
        return;
    }

    if (j.contains("bindings") && j["bindings"].is_array()) {
        for (const auto& bj : j["bindings"]) {
            InputBinding binding;
            binding.action = bj.value("action", "");
            if (binding.action.empty()) continue;

            // Single source or composite
            if (bj.contains("source")) {
                auto src_str = bj["source"].get<std::string>();

                // Expand shorthands for axis2d
                if (src_str == "pad_lstick" || src_str == "pad_rstick" ||
                    src_str == "mouse_move") {
                    // Store the _x variant; evaluation will read _x and _y
                    binding.source = input_source_from_string(src_str);
                } else {
                    binding.source = input_source_from_string(src_str);
                }
            }

            // Composite WASD-style
            if (bj.contains("up")) binding.up = input_source_from_string(bj["up"].get<std::string>());
            if (bj.contains("down")) binding.down = input_source_from_string(bj["down"].get<std::string>());
            if (bj.contains("left")) binding.left = input_source_from_string(bj["left"].get<std::string>());
            if (bj.contains("right")) binding.right = input_source_from_string(bj["right"].get<std::string>());

            // Modifiers
            if (bj.contains("modifiers") && bj["modifiers"].is_array()) {
                for (const auto& mj : bj["modifiers"]) {
                    InputModifier mod;
                    mod.type = parse_modifier_type(mj.value("type", "scale"));
                    mod.value = mj.value("value", 0.0f);
                    binding.modifiers.push_back(mod);
                }
            }

            ctx.bindings.push_back(std::move(binding));
        }
    }

    FJELL_CORE_INFO("InputActionMap: loaded context '{}' ({} bindings)",
                    ctx.name, ctx.bindings.size());
    contexts_[ctx.name] = std::move(ctx);
}

// ── Context stack ────────────────────────────────────────────────────

void InputActionMap::push_context(const std::string& name) {
    // Don't push duplicates
    for (const auto& s : context_stack_) {
        if (s == name) return;
    }
    context_stack_.push_back(name);
}

void InputActionMap::pop_context(const std::string& name) {
    for (auto it = context_stack_.begin(); it != context_stack_.end(); ++it) {
        if (*it == name) {
            context_stack_.erase(it);
            return;
        }
    }
}

// ── Reading raw input ────────────────────────────────────────────────

bool InputActionMap::read_source_held(InputSource source, const Input& raw) const {
    if (is_keyboard_source(source)) {
        return raw.is_key_held(static_cast<int>(source));
    }
    if (is_mouse_button_source(source)) {
        int btn = static_cast<int>(source) - static_cast<int>(InputSource::mouse_left);
        return raw.is_mouse_button_held(btn);
    }
    if (is_pad_button_source(source)) {
        int btn = static_cast<int>(source) - static_cast<int>(InputSource::pad_a);
        return raw.pad_button_held(btn);
    }
    // Triggers as buttons (held if > 0.5)
    if (source == InputSource::pad_lt || source == InputSource::pad_rt) {
        int axis = static_cast<int>(source) - static_cast<int>(InputSource::pad_lstick_x);
        return raw.pad_axis(axis) > 0.5f;
    }
    return false;
}

float InputActionMap::read_source_value(InputSource source, const Input& raw) const {
    if (is_keyboard_source(source)) {
        return raw.is_key_held(static_cast<int>(source)) ? 1.0f : 0.0f;
    }
    if (is_mouse_button_source(source)) {
        int btn = static_cast<int>(source) - static_cast<int>(InputSource::mouse_left);
        return raw.is_mouse_button_held(btn) ? 1.0f : 0.0f;
    }
    if (source == InputSource::mouse_move_x) return static_cast<float>(raw.mouse_delta().x);
    if (source == InputSource::mouse_move_y) return static_cast<float>(raw.mouse_delta().y);
    if (source == InputSource::mouse_scroll_x) return static_cast<float>(raw.scroll_delta().x);
    if (source == InputSource::mouse_scroll_y) return static_cast<float>(raw.scroll_delta().y);
    if (is_pad_button_source(source)) {
        int btn = static_cast<int>(source) - static_cast<int>(InputSource::pad_a);
        return raw.pad_button_held(btn) ? 1.0f : 0.0f;
    }
    if (is_pad_axis_source(source)) {
        int axis = static_cast<int>(source) - static_cast<int>(InputSource::pad_lstick_x);
        return raw.pad_axis(axis);
    }
    return 0.0f;
}

// ── Evaluation ───────────────────────────────────────────────────────

void InputActionMap::evaluate_binding(const InputBinding& binding, const Input& raw,
                                       std::unordered_map<std::string, bool>& consumed) {
    if (consumed.contains(binding.action)) return;

    auto it = states_.find(binding.action);
    if (it == states_.end()) return;
    auto& state = it->second;

    if (binding.is_composite()) {
        // Composite axis2d: 4 buttons → vec2
        float x = 0.0f;
        float y = 0.0f;
        if (read_source_held(binding.right, raw)) x += 1.0f;
        if (read_source_held(binding.left, raw)) x -= 1.0f;
        if (read_source_held(binding.up, raw)) y += 1.0f;
        if (read_source_held(binding.down, raw)) y -= 1.0f;

        glm::vec2 v{x, y};
        if (glm::length(v) > 1.0f) v = glm::normalize(v);

        if (!binding.modifiers.empty()) {
            v = apply_modifiers_2d(v, binding.modifiers.data(), binding.modifiers.size());
        }

        // Max magnitude wins
        if (glm::length(v) > glm::length(state.axis)) {
            state.axis = v;
        }
        if (glm::length(v) > 0.0f) {
            state.held = true;
        }
    } else if (state.type == ActionType::axis2d) {
        // Axis2d from analog source (e.g., pad_lstick)
        auto src = binding.source;
        InputSource src_y;

        // Expand shorthands
        if (src == InputSource::pad_lstick_x) src_y = InputSource::pad_lstick_y;
        else if (src == InputSource::pad_rstick_x) src_y = InputSource::pad_rstick_y;
        else if (src == InputSource::mouse_move_x) src_y = InputSource::mouse_move_y;
        else {
            // Single axis mapped to axis2d X
            float val = read_source_value(src, raw);
            if (!binding.modifiers.empty()) {
                val = apply_modifiers_1d(val, binding.modifiers.data(), binding.modifiers.size());
            }
            if (std::abs(val) > std::abs(state.axis.x)) {
                state.axis.x = val;
            }
            if (std::abs(val) > 0.0f) state.held = true;
            return;
        }

        glm::vec2 v{read_source_value(src, raw), read_source_value(src_y, raw)};
        if (!binding.modifiers.empty()) {
            v = apply_modifiers_2d(v, binding.modifiers.data(), binding.modifiers.size());
        }
        if (glm::length(v) > glm::length(state.axis)) {
            state.axis = v;
        }
        if (glm::length(v) > 0.0f) state.held = true;
    } else if (state.type == ActionType::axis1d) {
        float val = read_source_value(binding.source, raw);
        if (!binding.modifiers.empty()) {
            val = apply_modifiers_1d(val, binding.modifiers.data(), binding.modifiers.size());
        }
        if (std::abs(val) > std::abs(state.value)) {
            state.value = val;
        }
        if (std::abs(val) > 0.0f) state.held = true;
    } else {
        // Button
        bool held = read_source_held(binding.source, raw);
        if (held) state.held = true;
    }
}

void InputActionMap::update(const Input& raw_input) {
    // Save previous states for pressed/released + triggered/completed detection
    std::unordered_map<std::string, bool> prev_held;
    for (auto& [name, state] : states_) {
        prev_held[name] = state.held;
        state.pressed = false;
        state.released = false;
        state.held = false;
        state.value = 0.0f;
        state.axis = {0.0f, 0.0f};
    }

    // Walk context stack top-to-bottom
    std::unordered_map<std::string, bool> consumed;
    for (int i = static_cast<int>(context_stack_.size()) - 1; i >= 0; --i) {
        auto ctx_it = contexts_.find(context_stack_[i]);
        if (ctx_it == contexts_.end()) continue;

        const auto& ctx = ctx_it->second;
        for (const auto& binding : ctx.bindings) {
            evaluate_binding(binding, raw_input, consumed);
        }

        // Mark consumed actions
        if (ctx.consume) {
            for (const auto& binding : ctx.bindings) {
                auto sit = states_.find(binding.action);
                if (sit != states_.end() && sit->second.held) {
                    consumed[binding.action] = true;
                }
            }
        }
    }

    // Derive pressed/released and fire delegates
    for (auto& [name, state] : states_) {
        bool was = prev_held[name];
        bool is_active = false;

        if (state.type == ActionType::button) {
            is_active = state.held;
            state.pressed = state.held && !was;
            state.released = !state.held && was;
        } else if (state.type == ActionType::axis1d) {
            is_active = std::abs(state.value) > 0.0f;
        } else {
            is_active = glm::length(state.axis) > 0.0f;
        }

        // Fire delegates
        auto dit = delegates_.find(name);
        if (dit == delegates_.end()) continue;
        auto& d = dit->second;

        // Triggered / completed
        if (is_active && !state.was_active) {
            d.on_triggered.broadcast();
        }
        if (!is_active && state.was_active) {
            d.on_completed.broadcast();
        }
        state.was_active = is_active;

        // Button-specific
        if (state.pressed) d.on_pressed.broadcast();
        if (state.released) d.on_released.broadcast();

        // Continuous
        if (is_active) {
            if (state.type == ActionType::axis1d) {
                d.on_axis1d.broadcast(state.value);
            } else if (state.type == ActionType::axis2d) {
                d.on_axis2d.broadcast(state.axis);
            }
        }
    }

    // Listen mode: capture any input
    if (listening_) {
        // Check keyboard
        for (int k = 32; k < 349; ++k) {
            if (raw_input.is_key_pressed(k)) {
                captured_ = static_cast<InputSource>(k);
                listening_ = false;
                return;
            }
        }
        // Check mouse buttons
        for (int b = 0; b < 5; ++b) {
            if (raw_input.is_mouse_button_pressed(b)) {
                captured_ = static_cast<InputSource>(
                    static_cast<int>(InputSource::mouse_left) + b);
                listening_ = false;
                return;
            }
        }
        // Check gamepad buttons
        for (int b = 0; b < 15; ++b) {
            if (raw_input.pad_button_pressed(b)) {
                captured_ = static_cast<InputSource>(
                    static_cast<int>(InputSource::pad_a) + b);
                listening_ = false;
                return;
            }
        }
    }
}

// ── Polling queries ──────────────────────────────────────────────────

bool InputActionMap::is_pressed(const std::string& action) const {
    auto it = states_.find(action);
    return it != states_.end() && it->second.pressed;
}

bool InputActionMap::is_held(const std::string& action) const {
    auto it = states_.find(action);
    return it != states_.end() && it->second.held;
}

bool InputActionMap::is_released(const std::string& action) const {
    auto it = states_.find(action);
    return it != states_.end() && it->second.released;
}

float InputActionMap::axis1d(const std::string& action) const {
    auto it = states_.find(action);
    return it != states_.end() ? it->second.value : 0.0f;
}

glm::vec2 InputActionMap::axis2d(const std::string& action) const {
    auto it = states_.find(action);
    return it != states_.end() ? it->second.axis : glm::vec2{0.0f};
}

ActionDelegates* InputActionMap::delegates_for(const std::string& action) {
    auto it = delegates_.find(action);
    return it != delegates_.end() ? &it->second : nullptr;
}

// ── Rebinding ────────────────────────────────────────────────────────

void InputActionMap::rebind(const std::string& context, const std::string& action,
                             int binding_index, InputSource new_source) {
    auto it = contexts_.find(context);
    if (it == contexts_.end()) return;

    int idx = 0;
    for (auto& binding : it->second.bindings) {
        if (binding.action == action) {
            if (idx == binding_index) {
                binding.source = new_source;
                return;
            }
            ++idx;
        }
    }
}

void InputActionMap::save_context(const std::string& context, const std::string& path) const {
    auto it = contexts_.find(context);
    if (it == contexts_.end()) return;

    const auto& ctx = it->second;
    nlohmann::json j;
    j["name"] = ctx.name;
    j["consume"] = ctx.consume;

    auto& bindings_arr = j["bindings"];
    bindings_arr = nlohmann::json::array();

    for (const auto& b : ctx.bindings) {
        nlohmann::json bj;
        bj["action"] = b.action;

        if (b.is_composite()) {
            bj["up"] = input_source_to_string(b.up);
            bj["down"] = input_source_to_string(b.down);
            bj["left"] = input_source_to_string(b.left);
            bj["right"] = input_source_to_string(b.right);
        } else {
            bj["source"] = input_source_to_string(b.source);
        }

        if (!b.modifiers.empty()) {
            auto& marr = bj["modifiers"];
            marr = nlohmann::json::array();
            for (const auto& m : b.modifiers) {
                nlohmann::json mj;
                switch (m.type) {
                    case ModifierType::scale: mj["type"] = "scale"; break;
                    case ModifierType::deadzone: mj["type"] = "deadzone"; break;
                    case ModifierType::negate: mj["type"] = "negate"; break;
                    case ModifierType::swizzle: mj["type"] = "swizzle"; break;
                }
                if (m.type != ModifierType::negate && m.type != ModifierType::swizzle) {
                    mj["value"] = m.value;
                }
                marr.push_back(mj);
            }
        }

        bindings_arr.push_back(bj);
    }

    std::ofstream out(path);
    out << j.dump(4) << "\n";
}

// ── Listen mode ──────────────────────────────────────────────────────

void InputActionMap::start_listening() {
    listening_ = true;
    captured_ = InputSource::none;
}

void InputActionMap::cancel_listening() {
    listening_ = false;
    captured_ = InputSource::none;
}

} // namespace fjell
