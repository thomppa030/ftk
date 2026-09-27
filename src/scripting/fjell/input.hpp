// input.hpp — Input queries for Fjell game code
// Shipped to game projects alongside fjell/api.h. The engine's own definition
// of this class is core/input.hpp, under the same include guard. This one
// declares only the queries the engine exports and none of its data, so a game
// never depends on the engine's layout: every call lands in the engine's code.

#ifndef FJELL_INPUT_HPP
#define FJELL_INPUT_HPP

#include "fjell/api.h"

#include <glm/glm.hpp>

namespace fjell {

/// Raw keyboard and mouse state for the current frame, reached via
/// `ScriptComponent::input()`.
///
/// This is the direct-polling path — good for prototyping and for debug keys.
/// For shipping input prefer `[[fj::action]]` methods with `InputAPI`, so
/// players can remap and gamepads work without a second code path.
///
/// The key, button, delta and scroll queries read idle or zero while the
/// editor rather than the game has focus in docked play.
///
/// @code
/// void tick(float dt) override {
///     if (input().is_key_pressed(FJELL_KEY_SPACE)) jump();
///     if (input().is_key_held(FJELL_KEY_W))        walk_forward(dt);
/// }
/// @endcode
class Input {
public:
    /// The engine owns the one Input; a game holds it by reference. It
    /// cannot be made, copied or moved, since this declaration carries none
    /// of its state.
    Input() = delete;
    ~Input() = delete;
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;
    Input(Input&&) = delete;
    Input& operator=(Input&&) = delete;

    /// True only on the first frame the key goes down — the edge. Use for
    /// discrete actions like jumping or toggling.
    /// @param key A `FJELL_KEY_*` constant from `fjell/api.h`.
    [[nodiscard]] FJELL_SDK_EXPORT bool is_key_pressed(int key) const;

    /// True every frame the key is down, including the first. Use for
    /// continuous actions like walking.
    /// @param key A `FJELL_KEY_*` constant from `fjell/api.h`.
    [[nodiscard]] FJELL_SDK_EXPORT bool is_key_held(int key) const;

    /// True only on the frame the key comes back up.
    /// @param key A `FJELL_KEY_*` constant from `fjell/api.h`.
    [[nodiscard]] FJELL_SDK_EXPORT bool is_key_released(int key) const;

    /// True only on the first frame the button goes down.
    /// @param button `FJELL_MOUSE_LEFT`, `_RIGHT`, or `_MIDDLE`.
    [[nodiscard]] FJELL_SDK_EXPORT bool is_mouse_button_pressed(int button) const;

    /// True every frame the button is down.
    /// @param button `FJELL_MOUSE_LEFT`, `_RIGHT`, or `_MIDDLE`.
    [[nodiscard]] FJELL_SDK_EXPORT bool is_mouse_button_held(int button) const;

    /// Cursor position in pixels, origin at the window's top-left.
    /// Keeps its last value while the cursor is locked — use
    /// `mouse_delta()` for look controls instead.
    [[nodiscard]] FJELL_SDK_EXPORT glm::dvec2 mouse_position() const;

    /// Cursor movement in pixels since the previous frame. This is the value
    /// to drive camera look with; it keeps working while locked. Already a
    /// per-frame delta, so do NOT multiply it by delta time.
    [[nodiscard]] FJELL_SDK_EXPORT glm::dvec2 mouse_delta() const;

    /// Scroll wheel movement since the previous frame; `y` is the usual
    /// vertical wheel. Zero on frames without scrolling.
    [[nodiscard]] FJELL_SDK_EXPORT glm::dvec2 scroll_delta() const;

    /// Whether the cursor is locked to the window and hidden right now. A
    /// script locks it with `InputAPI::set_cursor_locked()`.
    [[nodiscard]] FJELL_SDK_EXPORT bool is_cursor_captured() const;
};

} // namespace fjell

#endif // FJELL_INPUT_HPP
