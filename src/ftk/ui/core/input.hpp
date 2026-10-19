#pragma once

// Input as the UI core takes it. Keys, buttons and pointer shapes are the
// kit's vocabulary (ftk/ui/kit/input.hpp includes them from here). Mouse,
// pen and each finger are pointers of one kind (decision 8 of the UI core
// plan): each has a position, pressure, tilt and twist, buttons, and
// whether it can hover, and a canvas gets every sample of one since the
// last frame.
//
// A program queues what its window reports, in order, and the next frame
// takes it:
//
//     ui.add_pointer_event({.kind = PointerEvent::Kind::Down, .type = PointerType::Pen, .id = pen,
//                           .root = window, .sample = {.position = at, .pressure = 0.4f, .time = t}});

#include <glm/vec2.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

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

/// How many keys there are.
inline constexpr std::size_t KEY_COUNT = static_cast<std::size_t>(Key::GraveAccent) + 1;

/// The modifier keys held.
struct Modifiers {
    bool ctrl{false};
    bool shift{false};
    bool alt{false};
    bool super{false};

    bool operator==(const Modifiers&) const = default;
};

/// A mouse's buttons. A pen's tip is Left and its barrel button Right; a
/// finger is Left.
enum class MouseButton : uint8_t { Left, Right, Middle };

/// The shapes the mouse pointer takes. Hidden shows none, a pen's pointer
/// included, for a program that draws its own where the pointer is: a
/// brush's outline, a game's crosshair.
enum class MouseCursor : uint8_t { Arrow, Text, Hand, ResizeAll, ResizeEW, ResizeNS, Hidden };

} // namespace ftk::ui

namespace ftk::ui::core {

/// The program's name for one of its OS windows.
using RootId = uint32_t;

enum class PointerType : uint8_t { Mouse, Pen, Touch };

/// A pointer's name among those of its type: the mouse is 0, a pen and a
/// finger go by the ids the system gives them.
using PointerId = uint64_t;

/// One reading of a pointer.
struct PointerSample {
    /// In layout units from the root's top-left.
    glm::vec2 position{0.0f};
    /// From 0 to 1; a mouse presses at 1.
    float pressure{1.0f};
    /// A pen's lean along x and y, from -1 to 1.
    glm::vec2 tilt{0.0f};
    /// A pen's turn about its own axis, in radians, where it says.
    float twist{0.0f};
    /// Seconds on the frame clock.
    double time{0.0};
};

struct PointerEvent {
    enum class Kind : uint8_t {
        /// Moved, or came into the window or near it (a pen in proximity).
        Move,
        /// A button, a pen's tip or a finger went down.
        Down,
        Up,
        /// A finger the system took back: its touch is undone, not let go.
        /// Android reports a palm so, after the fact.
        Cancel,
        /// Left the window, or a pen went out of proximity.
        Leave,
        /// The wheel turned, by `wheel` notches.
        Wheel,
    };
    Kind kind{Kind::Move};
    PointerType type{PointerType::Mouse};
    PointerId id{0};
    RootId root{0};
    MouseButton button{MouseButton::Left};
    PointerSample sample{};
    /// A pen's eraser end is down or near.
    bool eraser{false};
    /// Notches across and down; up and right are positive.
    glm::vec2 wheel{0.0f};
};

/// A key, or the modifiers alone.
struct KeyEvent {
    enum class Kind : uint8_t {
        Down,
        Up,
        /// Only the modifier keys changed.
        Modifiers,
    };
    Kind kind{Kind::Down};
    Key key{Key::A};
    /// Down again while held, at the keyboard's repeat rate.
    bool repeat{false};
    /// The modifiers held as it happened.
    Modifiers modifiers{};
    RootId root{0};
};

/// One of a frame's keystrokes: a key going down or repeating as it is
/// held, or text the system committed. A key that types text comes before
/// its text.
struct Keystroke {
    enum class Kind : uint8_t { Key, Text };
    Kind kind{Kind::Key};
    /// The key, for a key.
    Key key{Key::A};
    /// Down again while held, at the keyboard's repeat rate.
    bool repeat{false};
    /// The modifiers held as it happened.
    Modifiers modifiers{};
    /// What was typed, UTF-8, for text.
    std::string text{};

    bool operator==(const Keystroke&) const = default;
};

/// The distances and times pointers are judged by.
struct PointerStyle {
    /// How far a held mouse or pen moves before it drags.
    float drag_threshold{3.0f};
    /// The same for a finger, which wanders.
    float touch_drag_threshold{8.0f};
    /// The most between two presses that make a double click, in seconds
    /// and in layout units.
    double double_click_time{0.3};
    float double_click_distance{6.0f};
    /// How far one notch of the wheel scrolls.
    float wheel_step{48.0f};
    /// How long a still press lasts before it is a long press, which shows
    /// what something is where there is no hover.
    double long_press_time{0.5};
    /// The longest fingers may be down for their lifting to be a tap.
    double tap_time{0.3};
    /// How long the mouse rests on a widget before its tooltip shows
    /// (sheet 5).
    double tooltip_delay{0.15};
};

/// A pointer as the frame sees it.
struct Pointer {
    PointerType type{PointerType::Mouse};
    PointerId id{0};
    /// The window it is in.
    RootId root{0};
    /// Where it is now, and how.
    PointerSample now{};
    /// Where it was as the last frame ended: it has moved by now's position
    /// less this since. Where it came in, the frame it comes.
    glm::vec2 previous{0.0f};
    bool eraser{false};
    /// Touch has no hover: a finger is only somewhere while it is down.
    bool can_hover{true};
    /// Over the window (the mouse), near it (a pen), or down (a finger).
    bool present{false};
    /// The buttons held now, and those that went down and came up this
    /// frame, a bit each by MouseButton.
    uint8_t buttons{0};
    uint8_t pressed{0};
    uint8_t released{0};
    /// Taken back by the system this frame: what it started is dropped.
    bool cancelled{false};
    /// Where each button last went down, and where the pointer was let go.
    glm::vec2 press_position[3]{};
    glm::vec2 release_position{0.0f};
    /// Presses in quick succession at about the same place, per button.
    int click_count[3]{};
    double last_press_time[3]{};
    /// Every reading since the last frame, in order.
    std::vector<PointerSample> samples{};
    /// The wheel's turn this frame (the mouse).
    glm::vec2 wheel{0.0f};
};

[[nodiscard]] constexpr uint8_t button_bit(MouseButton button) {
    return static_cast<uint8_t>(1u << static_cast<unsigned>(button));
}

} // namespace ftk::ui::core
