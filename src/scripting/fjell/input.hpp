// fjell_input.hpp — Read-only Input interface for Fjell game code
// Shipped to game projects alongside fjell_api.h.
// Same data layout as core/input.hpp but only exposes const query methods.
// Uses shared include guard with core/input.hpp to prevent ODR issues.

#ifndef FJELL_INPUT_HPP
#define FJELL_INPUT_HPP

#include <glm/glm.hpp>

#include <array>

namespace fjell {

class Input {
public:
    [[nodiscard]] bool is_key_pressed(int key) const;
    [[nodiscard]] bool is_key_held(int key) const;
    [[nodiscard]] bool is_key_released(int key) const;

    [[nodiscard]] bool is_mouse_button_pressed(int button) const;
    [[nodiscard]] bool is_mouse_button_held(int button) const;

    [[nodiscard]] glm::dvec2 mouse_position() const { return mouse_pos_; }
    [[nodiscard]] glm::dvec2 mouse_delta() const { return mouse_delta_; }
    [[nodiscard]] glm::dvec2 scroll_delta() const { return scroll_delta_; }

    [[nodiscard]] bool is_cursor_captured() const { return cursor_captured_; }

private:
    static constexpr int MAX_KEYS = 512;
    static constexpr int MAX_BUTTONS = 8;

    enum class KeyState : uint8_t { Idle, Pressed, Held, Released };

    std::array<KeyState, MAX_KEYS> keys_{};
    std::array<KeyState, MAX_BUTTONS> mouse_buttons_{};

    glm::dvec2 mouse_pos_{0.0};
    glm::dvec2 mouse_last_{0.0};
    glm::dvec2 mouse_delta_{0.0};
    glm::dvec2 scroll_delta_{0.0};
    bool first_mouse_{true};
    bool cursor_captured_{false};
};

} // namespace fjell

#endif // FJELL_INPUT_HPP
