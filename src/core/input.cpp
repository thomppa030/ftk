#include "core/input.hpp"
#include "fjell/api.h"
#include "core/base_path.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "renderer/gpu/window.hpp"

#include <GLFW/glfw3.h>

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>

namespace fjell {

namespace {

/// Feed one SDL-format mapping file to GLFW. Returns false when the file is
/// missing so the caller can tell "absent" from "present but rejected".
bool load_mapping_file(const std::string& relative) {
    auto path = base_path::resolve(relative);
    std::ifstream file(path);
    if (!file) return false;

    std::stringstream buffer;
    buffer << file.rdbuf();
    const auto text = buffer.str();
    if (text.empty()) return false;

    if (glfwUpdateGamepadMappings(text.c_str()) == GLFW_FALSE) {
        FJELL_CORE_WARN("Gamepad mappings rejected by GLFW: {}", path);
        return false;
    }
    return true;
}

/// Build an SDL mapping line for a joystick that reports the XInput layout
/// but has no database entry.
///
/// GLFW resolves controllers strictly by GUID and treats a miss as "not a
/// gamepad", so an unlisted pad is invisible rather than merely unmapped.
/// SDL avoids that with a generic entry covering anything the kernel's xpad
/// driver claims, and every clone of that shape reports its controls in the
/// same order. Recreate that here: when the counts match, assume the standard
/// layout rather than requiring someone to hand-write a GUID entry.
///
/// Deliberately conservative — an exact shape match only. A device with a
/// different count gets left unmapped and logged, because guessing at an
/// unknown layout produces silently wrong buttons, which is worse than a pad
/// that visibly does not work.
std::string synthesize_xinput_mapping(int jid) {
    // The xpad shape: eleven face/shoulder/stick/menu buttons, two sticks,
    // two analog triggers, and the d-pad delivered as a hat.
    //
    // glfwGetJoystickButtons appends each hat as four more buttons for
    // backward compatibility, so the count it reports is the eleven real
    // ones plus four for the d-pad.
    constexpr int XINPUT_HATS = 1;
    constexpr int XINPUT_REAL_BUTTONS = 11;
    constexpr int XINPUT_BUTTONS = XINPUT_REAL_BUTTONS + (4 * XINPUT_HATS);
    constexpr int XINPUT_AXES = 6;

    int button_count = 0;
    int axis_count = 0;
    int hat_count = 0;
    glfwGetJoystickButtons(jid, &button_count);
    glfwGetJoystickAxes(jid, &axis_count);
    glfwGetJoystickHats(jid, &hat_count);

    if (button_count != XINPUT_BUTTONS || axis_count != XINPUT_AXES
        || hat_count != XINPUT_HATS) {
        return {};
    }

    const char* guid = glfwGetJoystickGUID(jid);
    const char* name = glfwGetJoystickName(jid);
    if (guid == nullptr) return {};

    std::string mapping = guid;
    mapping += ',';
    mapping += (name != nullptr) ? name : "Generic Gamepad";
    mapping +=
        ",a:b0,b:b1,x:b2,y:b3"
        ",leftshoulder:b4,rightshoulder:b5"
        ",back:b6,start:b7,guide:b8"
        ",leftstick:b9,rightstick:b10"
        ",leftx:a0,lefty:a1,rightx:a3,righty:a4"
        ",lefttrigger:a2,righttrigger:a5"
        ",dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8"
        ",platform:Linux,";
    return mapping;
}

/// Give one joystick a mapping if it does not already have one, and report
/// what happened. Returns true when it ends up usable as a gamepad.
///
/// Runs for pads present at startup and again for any that appear later, so
/// a controller plugged in mid-session gets the same fallback treatment.
bool ensure_joystick_mapped(int jid) {
    if (glfwJoystickIsGamepad(jid) == GLFW_TRUE) return true;

    const char* name = glfwGetJoystickName(jid);
    const char* guid = glfwGetJoystickGUID(jid);

    auto fallback = synthesize_xinput_mapping(jid);
    if (!fallback.empty()
        && glfwUpdateGamepadMappings(fallback.c_str()) == GLFW_TRUE
        && glfwJoystickIsGamepad(jid) == GLFW_TRUE) {
        FJELL_CORE_INFO("Joystick {} '{}' matched the XInput layout; using a "
                        "generic mapping (GUID {})",
                        jid, name != nullptr ? name : "?",
                        guid != nullptr ? guid : "?");
        return true;
    }

    FJELL_CORE_WARN("Joystick {} '{}' has no gamepad mapping (GUID {}) — add "
                    "one to engine_assets/input/gamecontrollerdb_extra.txt",
                    jid, name != nullptr ? name : "?",
                    guid != nullptr ? guid : "?");
    return false;
}

} // namespace

void Input::load_gamepad_mappings() {
    // Upstream database first, then local additions, so a GUID present in
    // both resolves to ours.
    if (!load_mapping_file("engine_assets/input/gamecontrollerdb.txt")) {
        FJELL_CORE_WARN("Gamepad database missing — only GLFW's built-in "
                        "mappings are available");
    }
    load_mapping_file("engine_assets/input/gamecontrollerdb_extra.txt");

    // Map whatever is already attached. Which one actually gets read is
    // decided per frame by resolve_pad(), which reports it — no need to
    // announce them twice here.
    for (int jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_LAST; ++jid) {
        if (glfwJoystickPresent(jid) == GLFW_FALSE) continue;
        ensure_joystick_mapped(jid);
    }
}

namespace {

/// The Input attached to a GLFW window, found through the window's user
/// pointer, or null when none is.
Input* input_of(GLFWwindow* handle) {
    auto* owner = static_cast<Window*>(glfwGetWindowUserPointer(handle));
    return owner != nullptr ? owner->input() : nullptr;
}

} // namespace

void Input::attach(Window& window) {
    window_ = &window;
    window.set_input(this);

    GLFWwindow* handle = window.handle();
    glfwSetKeyCallback(handle, [](GLFWwindow* w, int key, int /*scancode*/,
                                  int action, int /*mods*/) {
        auto* input = input_of(w);
        if (input == nullptr || key < 0 || key >= MAX_KEYS) return;

        if (action == GLFW_PRESS) {
            input->keys_[key] = KeyState::Pressed;
        } else if (action == GLFW_RELEASE) {
            input->keys_[key] = KeyState::Released;
        }
    });
    glfwSetCursorPosCallback(handle, [](GLFWwindow* w, double xpos, double ypos) {
        auto* input = input_of(w);
        if (input == nullptr) return;

        glm::dvec2 pos{xpos, ypos};

        if (input->first_mouse_) {
            input->mouse_last_ = pos;
            input->first_mouse_ = false;
        }

        input->mouse_delta_ += pos - input->mouse_last_;
        input->mouse_last_ = pos;
        input->mouse_pos_ = pos;
    });
    glfwSetMouseButtonCallback(handle, [](GLFWwindow* w, int button, int action,
                                          int /*mods*/) {
        auto* input = input_of(w);
        if (input == nullptr || button < 0 || button >= MAX_BUTTONS) return;

        if (action == GLFW_PRESS) {
            input->mouse_buttons_[button] = KeyState::Pressed;
        } else if (action == GLFW_RELEASE) {
            input->mouse_buttons_[button] = KeyState::Released;
        }
    });
    glfwSetScrollCallback(handle, [](GLFWwindow* w, double xoffset, double yoffset) {
        auto* input = input_of(w);
        if (input == nullptr) return;

        input->scroll_delta_ += glm::dvec2{xoffset, yoffset};
    });
}

void Input::resolve_pad() {
    // Keep the pad already being read for as long as it stays usable, so a
    // second controller connecting cannot take over mid-session.
    if (pad_jid_ >= 0 && glfwJoystickIsGamepad(pad_jid_) == GLFW_TRUE) {
        return;
    }

    if (pad_jid_ >= 0) {
        FJELL_CORE_INFO("Gamepad disconnected: {}",
                        pad_name_.empty() ? "?" : pad_name_);
        pad_jid_ = -1;
        pad_name_.clear();
    }

    // Scan every slot rather than assuming the first: a pad can be seated on
    // another port, and one connected after startup lands wherever GLFW puts
    // it.
    for (int jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_LAST; ++jid) {
        if (glfwJoystickPresent(jid) == GLFW_FALSE) {
            reported_unmapped_[jid] = false;
            continue;
        }

        if (glfwJoystickIsGamepad(jid) == GLFW_FALSE) {
            // This runs every frame while no pad is usable, so only try to
            // map a given slot once per connection — otherwise an unmappable
            // joystick warns forever.
            if (reported_unmapped_[jid]) continue;
            reported_unmapped_[jid] = true;
            if (!ensure_joystick_mapped(jid)) continue;
        }

        pad_jid_ = jid;
        const char* name = glfwGetGamepadName(jid);
        pad_name_ = name != nullptr ? name : "?";
        FJELL_CORE_INFO("Gamepad connected: {} (slot {})", pad_name_, jid);
        return;
    }
}

void Input::detect_active_device() {
    // Switching repaints every prompt, so it takes more than the deadzone a
    // bound action moves on: a resting stick drifts, and a mouse picks up
    // desk vibration. Both would otherwise flip the device back and forth
    // while the player is not touching either.
    constexpr float PAD_STICK_SWITCH = 0.5F;
    constexpr float PAD_TRIGGER_SWITCH = 0.3F;
    constexpr double MOUSE_MOVE_SWITCH = 8.0;  // pixels in one frame

    auto pad_active = [&] {
        if (!pad_connected_) return false;
        for (int i = 0; i < MAX_PAD_BUTTONS; ++i) {
            if (pad_buttons_[i] != 0U) return true;
        }
        // Sticks are checked by magnitude so a diagonal push counts the same
        // as an axis-aligned one.
        for (int axis = 0; axis < 4; axis += 2) {
            float x = pad_axes_[axis];
            float y = pad_axes_[axis + 1];
            if (std::sqrt((x * x) + (y * y)) > PAD_STICK_SWITCH) return true;
        }
        // Triggers rest at -1 and read 1 fully pressed.
        for (int axis = 4; axis < MAX_PAD_AXES; ++axis) {
            if ((pad_axes_[axis] + 1.0F) * 0.5F > PAD_TRIGGER_SWITCH) return true;
        }
        return false;
    };

    auto keyboard_mouse_active = [&] {
        for (auto key : keys_) {
            if (key == KeyState::Pressed || key == KeyState::Held) return true;
        }
        for (auto btn : mouse_buttons_) {
            if (btn == KeyState::Pressed || btn == KeyState::Held) return true;
        }
        if (std::abs(mouse_delta_.x) > MOUSE_MOVE_SWITCH
            || std::abs(mouse_delta_.y) > MOUSE_MOVE_SWITCH) {
            return true;
        }
        return scroll_delta_.x != 0.0 || scroll_delta_.y != 0.0;
    };

    // Only the device that is not already active can take over, so holding a
    // key does not keep re-asserting a device that already owns the state.
    auto wanted = active_device_;
    if (active_device_ == InputDevice::keyboard_mouse) {
        if (pad_active()) wanted = InputDevice::gamepad;
    } else if (keyboard_mouse_active()) {
        wanted = InputDevice::keyboard_mouse;
    }

    // A pad that goes away cannot stay the active device.
    if (!pad_connected_) wanted = InputDevice::keyboard_mouse;

    if (wanted != active_device_) {
        active_device_ = wanted;
        on_device_changed.broadcast(active_device_);
    }
}

void Input::update() {
    FJELL_PROFILE_SCOPE_N("input_update");
    for (auto& key : keys_) {
        if (key == KeyState::Pressed) {
            key = KeyState::Held;
        } else if (key == KeyState::Released) {
            key = KeyState::Idle;
        }
    }
    for (auto& btn : mouse_buttons_) {
        if (btn == KeyState::Pressed) {
            btn = KeyState::Held;
        } else if (btn == KeyState::Released) {
            btn = KeyState::Idle;
        }
    }
    // Gamepad polling
    pad_buttons_prev_ = pad_buttons_;
    resolve_pad();

    GLFWgamepadstate gamepad;
    if (pad_jid_ >= 0 && glfwGetGamepadState(pad_jid_, &gamepad) == GLFW_TRUE) {
        pad_connected_ = true;
        for (int i = 0; i < MAX_PAD_BUTTONS; ++i) {
            pad_buttons_[i] = gamepad.buttons[i];
        }
        for (int i = 0; i < MAX_PAD_AXES; ++i) {
            pad_axes_[i] = gamepad.axes[i];
        }
    } else {
        pad_connected_ = false;
        pad_buttons_.fill(0);
        pad_axes_.fill(0.0F);
    }

    // Runs while the mouse deltas still hold what the callbacks accumulated
    // during the last poll_events(); they are cleared for the next frame
    // immediately below.
    detect_active_device();

    mouse_delta_ = {0.0, 0.0};
    scroll_delta_ = {0.0, 0.0};
}

FJELL_SDK_EXPORT bool Input::is_key_pressed(int key) const {
    if (!game_focus_) return false;
    return raw_key_pressed(key);
}

FJELL_SDK_EXPORT bool Input::is_key_held(int key) const {
    if (!game_focus_) return false;
    return raw_key_held(key);
}

FJELL_SDK_EXPORT bool Input::is_key_released(int key) const {
    if (!game_focus_) return false;
    if (key < 0 || key >= MAX_KEYS) return false;
    return keys_[key] == KeyState::Released;
}

FJELL_SDK_EXPORT bool Input::is_mouse_button_pressed(int button) const {
    if (!game_focus_) return false;
    return raw_mouse_button_pressed(button);
}

FJELL_SDK_EXPORT bool Input::is_mouse_button_held(int button) const {
    if (!game_focus_) return false;
    return raw_mouse_button_held(button);
}

bool Input::raw_mouse_button_pressed(int button) const {
    if (button < 0 || button >= MAX_BUTTONS) return false;
    return mouse_buttons_[button] == KeyState::Pressed;
}

bool Input::raw_key_pressed(int key) const {
    if (key < 0 || key >= MAX_KEYS) return false;
    return keys_[key] == KeyState::Pressed;
}

bool Input::raw_key_held(int key) const {
    if (key < 0 || key >= MAX_KEYS) return false;
    auto state = keys_[key];
    return state == KeyState::Held || state == KeyState::Pressed;
}

bool Input::raw_mouse_button_held(int button) const {
    if (button < 0 || button >= MAX_BUTTONS) return false;
    auto state = mouse_buttons_[button];
    return state == KeyState::Held || state == KeyState::Pressed;
}

FJELL_SDK_EXPORT glm::dvec2 Input::mouse_position() const {
    return mouse_pos_;
}

FJELL_SDK_EXPORT glm::dvec2 Input::mouse_delta() const {
    return game_focus_ ? mouse_delta_ : glm::dvec2{0.0};
}

FJELL_SDK_EXPORT glm::dvec2 Input::scroll_delta() const {
    return game_focus_ ? scroll_delta_ : glm::dvec2{0.0};
}

FJELL_SDK_EXPORT bool Input::is_cursor_captured() const {
    return cursor_captured_;
}

void Input::set_cursor_captured(bool captured) {
    if (window_ == nullptr) return;
    cursor_captured_ = captured;
    window_->set_cursor_captured(captured);
    if (captured) {
        first_mouse_ = true;
    }
}

void Input::apply_cursor_lock(bool allowed) {
    const bool locked = cursor_lock_wanted_ && allowed;
    if (locked == cursor_captured_ || window_ == nullptr) {
        return;
    }
    set_cursor_captured(locked);
}

bool Input::pad_button_held(int button) const {
    if (!game_focus_) return false;
    if (button < 0 || button >= MAX_PAD_BUTTONS) return false;
    return pad_buttons_[button] != 0;
}

bool Input::pad_button_pressed(int button) const {
    if (!game_focus_) return false;
    return raw_pad_button_pressed(button);
}

bool Input::raw_pad_button_pressed(int button) const {
    if (button < 0 || button >= MAX_PAD_BUTTONS) return false;
    return pad_buttons_[button] != 0 && pad_buttons_prev_[button] == 0;
}

float Input::pad_axis(int axis) const {
    if (!game_focus_) return 0.0f;
    if (axis < 0 || axis >= MAX_PAD_AXES) return 0.0f;
    return pad_axes_[axis];
}

} // namespace fjell
