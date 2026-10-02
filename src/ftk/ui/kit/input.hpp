#pragma once

// The keyboard, the mouse and the clock, as the UI sees them this frame.
//
//     if (ui::modifiers().ctrl && ui::key_pressed(ui::Key::S)) save();
//     if (ui::item_hovered() && ui::mouse_clicked(ui::MouseButton::Right)) open_menu();

#include <glm/vec2.hpp>

#include <cstdint>
#include <string>

namespace ftk::ui {

enum class Key : uint8_t {
    A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Digit0, Digit1, Digit2, Digit3, Digit4, Digit5, Digit6, Digit7, Digit8, Digit9,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Escape, Enter, Tab, Space, Backspace, Delete, Insert,
    Home, End, PageUp, PageDown, Left, Right, Up, Down,
    Minus, Equal, LeftBracket, RightBracket, Backslash, Semicolon, Apostrophe, Comma, Period,
    Slash, GraveAccent,
};

/// `key` went down this frame.
[[nodiscard]] bool key_pressed(Key key);

/// The same, and again at the keyboard's repeat rate while it is held: for
/// what steps, such as moving through a list.
[[nodiscard]] bool key_repeated(Key key);

/// Keeps `key` for the shortcut `owner` names, so keyboard navigation
/// leaves it alone (End would otherwise jump to the window's last item).
/// The claim lapses on a frame the key is not held, so it is made every
/// frame the shortcut is live, before the key is read.
void claim_key(Key key, const char* owner);

/// The modifier keys held now.
struct Modifiers {
    bool ctrl{false};
    bool shift{false};
    bool alt{false};
    bool super{false};
};
[[nodiscard]] Modifiers modifiers();

/// A text field has the keyboard: keys are its, not shortcuts.
[[nodiscard]] bool typing();

/// Some key went down this frame, any of them.
[[nodiscard]] bool any_key_pressed();

/// While true the UI takes no keys at all, as when a game being played has
/// them; key_pressed() and the rest stay false.
void set_keyboard_ignored(bool ignored);

/// Puts `text` on the system clipboard, as a copy does.
void set_clipboard(const std::string& text);

/// What the system clipboard holds, as text; empty when it holds none.
[[nodiscard]] std::string clipboard();

enum class MouseButton : uint8_t { Left, Right, Middle };

/// `button` went down this frame.
[[nodiscard]] bool mouse_clicked(MouseButton button);
/// `button` went down this frame for the second time in quick succession.
[[nodiscard]] bool mouse_double_clicked(MouseButton button);
/// `button` is held.
[[nodiscard]] bool mouse_down(MouseButton button);
/// `button` came up this frame.
[[nodiscard]] bool mouse_released(MouseButton button);
/// `button` is held and the mouse has moved since it went down.
[[nodiscard]] bool mouse_dragging(MouseButton button);
/// Some mouse button came up this frame, any of them.
[[nodiscard]] bool any_mouse_released();

/// The shapes the mouse pointer takes.
enum class MouseCursor : uint8_t { Arrow, Text, Hand, ResizeAll, ResizeEW, ResizeNS };

/// The pointer's shape for this frame: over a place that types, drags or
/// resizes, to say so before it is pressed.
void set_mouse_cursor(MouseCursor cursor);

/// Where the mouse is, in screen pixels.
[[nodiscard]] glm::vec2 mouse_pos();

/// The mouse is in the rectangle from `min` to `max`, in the part of it
/// the window being drawn shows.
[[nodiscard]] bool mouse_over(glm::vec2 min, glm::vec2 max);

/// How far the wheel turned this frame, in notches: up is positive.
[[nodiscard]] float mouse_wheel();

/// Seconds since the UI began.
[[nodiscard]] double seconds();

/// Seconds since the last frame.
[[nodiscard]] float frame_seconds();

} // namespace ftk::ui
