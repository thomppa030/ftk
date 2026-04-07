#include "core/input.hpp"
#include "fjell/api.h"
#include "core/profiler.hpp"
#include "core/window.hpp"

#include <GLFW/glfw3.h>

namespace fjell {

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
