#include "core/input.hpp"
#include "fjell/api.h"
#include "core/base_path.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "core/window.hpp"

#include <GLFW/glfw3.h>

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

} // namespace

void Input::load_gamepad_mappings() {
    // Upstream database first, then local additions, so a GUID present in
    // both resolves to ours.
    if (!load_mapping_file("engine_assets/input/gamecontrollerdb.txt")) {
        FJELL_CORE_WARN("Gamepad database missing — only GLFW's built-in "
                        "mappings are available");
    }
    load_mapping_file("engine_assets/input/gamecontrollerdb_extra.txt");

    // Report what is attached, and fall back to the XInput layout for
    // anything still unmapped. A joystick without a mapping is invisible to
    // glfwGetGamepadState, which looks exactly like no controller at all, so
    // whatever is left over gets logged with its GUID.
    for (int jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_LAST; ++jid) {
        if (glfwJoystickPresent(jid) == GLFW_FALSE) continue;

        const char* name = glfwGetJoystickName(jid);
        const char* guid = glfwGetJoystickGUID(jid);

        if (glfwJoystickIsGamepad(jid) == GLFW_FALSE) {
            auto fallback = synthesize_xinput_mapping(jid);
            if (!fallback.empty()
                && glfwUpdateGamepadMappings(fallback.c_str()) == GLFW_TRUE) {
                FJELL_CORE_INFO("Joystick {} '{}' matched the XInput layout; "
                                "using a generic mapping (GUID {})",
                                jid, name != nullptr ? name : "?",
                                guid != nullptr ? guid : "?");
            }
        }

        if (glfwJoystickIsGamepad(jid) == GLFW_TRUE) {
            const char* pad_name = glfwGetGamepadName(jid);
            FJELL_CORE_INFO("Gamepad {}: {}", jid,
                            pad_name != nullptr ? pad_name : "?");
        } else {
            FJELL_CORE_WARN("Joystick {} '{}' has no gamepad mapping (GUID {}) "
                            "— add one to "
                            "engine_assets/input/gamecontrollerdb_extra.txt",
                            jid, name != nullptr ? name : "?",
                            guid != nullptr ? guid : "?");
        }
    }
}

void Input::attach(GLFWwindow* window) {
    window_ = window;
    glfwSetKeyCallback(window, key_callback);
    glfwSetCursorPosCallback(window, cursor_callback);
    glfwSetMouseButtonCallback(window, mouse_button_callback);
    glfwSetScrollCallback(window, scroll_callback);
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
    mouse_delta_ = {0.0, 0.0};
    scroll_delta_ = {0.0, 0.0};

    // Gamepad polling
    pad_buttons_prev_ = pad_buttons_;
    GLFWgamepadstate gamepad;
    if (glfwGetGamepadState(GLFW_JOYSTICK_1, &gamepad)) {
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
        pad_axes_.fill(0.0f);
    }
}

FJELL_SDK_EXPORT bool Input::is_key_pressed(int key) const {
    if (key < 0 || key >= MAX_KEYS) return false;
    return keys_[key] == KeyState::Pressed;
}

bool Input::is_key_held(int key) const {
    if (key < 0 || key >= MAX_KEYS) return false;
    auto state = keys_[key];
    return state == KeyState::Held || state == KeyState::Pressed;
}

bool Input::is_key_released(int key) const {
    if (key < 0 || key >= MAX_KEYS) return false;
    return keys_[key] == KeyState::Released;
}

bool Input::is_mouse_button_pressed(int button) const {
    if (button < 0 || button >= MAX_BUTTONS) return false;
    return mouse_buttons_[button] == KeyState::Pressed;
}

bool Input::is_mouse_button_held(int button) const {
    if (button < 0 || button >= MAX_BUTTONS) return false;
    auto state = mouse_buttons_[button];
    return state == KeyState::Held || state == KeyState::Pressed;
}

void Input::set_cursor_captured(GLFWwindow* window, bool captured) {
    cursor_captured_ = captured;
    glfwSetInputMode(window, GLFW_CURSOR,
                     captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    if (captured) {
        first_mouse_ = true;
    }
}

static Input* get_input(GLFWwindow* window) {
    auto* win = static_cast<Window*>(glfwGetWindowUserPointer(window));
    return win ? win->input() : nullptr;
}

void Input::key_callback(GLFWwindow* window, int key, int /*scancode*/,
                          int action, int /*mods*/) {
    auto* input = get_input(window);
    if (!input || key < 0 || key >= MAX_KEYS) return;

    if (action == GLFW_PRESS) {
        input->keys_[key] = KeyState::Pressed;
    } else if (action == GLFW_RELEASE) {
        input->keys_[key] = KeyState::Released;
    }
}

void Input::cursor_callback(GLFWwindow* window, double xpos, double ypos) {
    auto* input = get_input(window);
    if (!input) return;

    glm::dvec2 pos{xpos, ypos};

    if (input->first_mouse_) {
        input->mouse_last_ = pos;
        input->first_mouse_ = false;
    }

    input->mouse_delta_ += pos - input->mouse_last_;
    input->mouse_last_ = pos;
    input->mouse_pos_ = pos;
}

void Input::mouse_button_callback(GLFWwindow* window, int button,
                                   int action, int /*mods*/) {
    auto* input = get_input(window);
    if (!input || button < 0 || button >= MAX_BUTTONS) return;

    if (action == GLFW_PRESS) {
        input->mouse_buttons_[button] = KeyState::Pressed;
    } else if (action == GLFW_RELEASE) {
        input->mouse_buttons_[button] = KeyState::Released;
    }
}

void Input::scroll_callback(GLFWwindow* window, double xoffset, double yoffset) {
    auto* input = get_input(window);
    if (!input) return;

    input->scroll_delta_ += glm::dvec2{xoffset, yoffset};
}

bool Input::pad_button_held(int button) const {
    if (button < 0 || button >= MAX_PAD_BUTTONS) return false;
    return pad_buttons_[button] != 0;
}

bool Input::pad_button_pressed(int button) const {
    if (button < 0 || button >= MAX_PAD_BUTTONS) return false;
    return pad_buttons_[button] != 0 && pad_buttons_prev_[button] == 0;
}

float Input::pad_axis(int axis) const {
    if (axis < 0 || axis >= MAX_PAD_AXES) return 0.0f;
    return pad_axes_[axis];
}

} // namespace fjell
