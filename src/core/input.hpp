#ifndef FJELL_INPUT_HPP
#define FJELL_INPUT_HPP

#include "fjell/api.h"
#include <glm/glm.hpp>

#include <array>

struct GLFWwindow;

namespace fjell {

class Input {
public:
    void attach(GLFWwindow* window);
    void update();

    [[nodiscard]] GLFWwindow* window() const { return window_; }

    [[nodiscard]] FJELL_SDK_EXPORT bool is_key_pressed(int key) const;
    [[nodiscard]] bool is_key_held(int key) const;
    [[nodiscard]] bool is_key_released(int key) const;

    [[nodiscard]] bool is_mouse_button_pressed(int button) const;
    [[nodiscard]] bool is_mouse_button_held(int button) const;

    [[nodiscard]] glm::dvec2 mouse_position() const { return mouse_pos_; }
    [[nodiscard]] glm::dvec2 mouse_delta() const { return mouse_delta_; }
    [[nodiscard]] glm::dvec2 scroll_delta() const { return scroll_delta_; }

    void set_cursor_captured(GLFWwindow* window, bool captured);
    [[nodiscard]] bool is_cursor_captured() const { return cursor_captured_; }

    /// Load SDL gamepad mappings so controllers GLFW does not know are
    /// still usable. Call once before the first update(). Without a mapping
    /// glfwGetGamepadState fails outright and the pad reads as absent.
    static void load_gamepad_mappings();

    // Gamepad
    [[nodiscard]] bool pad_connected() const { return pad_connected_; }
    [[nodiscard]] bool pad_button_held(int button) const;
    [[nodiscard]] bool pad_button_pressed(int button) const;
    [[nodiscard]] float pad_axis(int axis) const;

private:
    static constexpr int MAX_KEYS = 512;
    static constexpr int MAX_BUTTONS = 8;
    static constexpr int MAX_PAD_BUTTONS = 15;
    static constexpr int MAX_PAD_AXES = 6;

    enum class KeyState : uint8_t { Idle, Pressed, Held, Released };

    std::array<KeyState, MAX_KEYS> keys_{};
    std::array<KeyState, MAX_BUTTONS> mouse_buttons_{};

    // Gamepad state
    std::array<uint8_t, MAX_PAD_BUTTONS> pad_buttons_{};
    std::array<uint8_t, MAX_PAD_BUTTONS> pad_buttons_prev_{};
    std::array<float, MAX_PAD_AXES> pad_axes_{};
    bool pad_connected_{false};

    glm::dvec2 mouse_pos_{0.0};
    glm::dvec2 mouse_last_{0.0};
    glm::dvec2 mouse_delta_{0.0};
    glm::dvec2 scroll_delta_{0.0};
    GLFWwindow* window_{nullptr};
    bool first_mouse_{true};
    bool cursor_captured_{false};

    static void key_callback(GLFWwindow* window, int key, int scancode,
                             int action, int mods);
    static void cursor_callback(GLFWwindow* window, double xpos, double ypos);
    static void mouse_button_callback(GLFWwindow* window, int button,
                                      int action, int mods);
    static void scroll_callback(GLFWwindow* window, double xoffset, double yoffset);
};

} // namespace fjell

#endif // FJELL_INPUT_HPP
