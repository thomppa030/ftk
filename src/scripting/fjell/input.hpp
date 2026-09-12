// fjell_input.hpp — Input interface for Fjell game code
// Shipped to game projects alongside fjell_api.h.
// Same data layout as core/input.hpp but only exposes query methods
// and cursor capture. Uses shared include guard with core/input.hpp
// to prevent ODR issues.

#ifndef FJELL_INPUT_HPP
#define FJELL_INPUT_HPP

#include "fjell/api.h"

#include <glm/glm.hpp>

#include <array>

struct GLFWwindow;

extern "C" FJELL_SDK_EXPORT void fjell_input_set_cursor_captured(bool captured);

namespace fjell {

/// Raw keyboard and mouse state for the current frame, reached via
/// `ScriptComponent::input()`.
///
/// This is the direct-polling path — good for prototyping and for debug keys.
/// For shipping input prefer `[[fj::action]]` methods with `InputAPI`, so
/// players can remap and gamepads work without a second code path.
///
/// @code
/// void tick(float dt) override {
///     if (input().is_key_pressed(FJELL_KEY_SPACE)) jump();
///     if (input().is_key_held(FJELL_KEY_W))        walk_forward(dt);
/// }
/// @endcode
class Input {
public:
    /// True only on the first frame the key goes down — the edge. Use for
    /// discrete actions like jumping or toggling.
    /// @param key A `FJELL_KEY_*` constant from `fjell/api.h`.
    [[nodiscard]] FJELL_SDK_EXPORT bool is_key_pressed(int key) const;

    /// True every frame the key is down, including the first. Use for
    /// continuous actions like walking.
    /// @param key A `FJELL_KEY_*` constant from `fjell/api.h`.
    [[nodiscard]] bool is_key_held(int key) const;

    /// True only on the frame the key comes back up.
    /// @param key A `FJELL_KEY_*` constant from `fjell/api.h`.
    [[nodiscard]] bool is_key_released(int key) const;

    /// True only on the first frame the button goes down.
    /// @param button `FJELL_MOUSE_LEFT`, `_RIGHT`, or `_MIDDLE`.
    [[nodiscard]] bool is_mouse_button_pressed(int button) const;

    /// True every frame the button is down.
    /// @param button `FJELL_MOUSE_LEFT`, `_RIGHT`, or `_MIDDLE`.
    [[nodiscard]] bool is_mouse_button_held(int button) const;

    /// Cursor position in pixels, origin at the window's top-left.
    /// Keeps its last value while the cursor is captured — use
    /// `mouse_delta()` for look controls instead.
    [[nodiscard]] glm::dvec2 mouse_position() const { return mouse_pos_; }

    /// Cursor movement in pixels since the previous frame. This is the value
    /// to drive camera look with; it keeps working while captured. Already a
    /// per-frame delta, so do NOT multiply it by delta time.
    [[nodiscard]] glm::dvec2 mouse_delta() const { return mouse_delta_; }

    /// Scroll wheel movement since the previous frame; `y` is the usual
    /// vertical wheel. Zero on frames without scrolling.
    [[nodiscard]] glm::dvec2 scroll_delta() const { return scroll_delta_; }

    /// Whether the cursor is currently locked to the window and hidden.
    [[nodiscard]] bool is_cursor_captured() const { return cursor_captured_; }

    /// Lock and hide the cursor for mouse-look, or release it for menus.
    /// While captured, `mouse_delta()` keeps reporting movement but
    /// `mouse_position()` stops changing.
    void set_cursor_captured(bool captured) {
        fjell_input_set_cursor_captured(captured);
    }

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
    GLFWwindow* window_{nullptr};
    bool first_mouse_{true};
    bool cursor_captured_{false};
};

} // namespace fjell

#endif // FJELL_INPUT_HPP
