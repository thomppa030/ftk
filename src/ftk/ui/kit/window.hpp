#pragma once

// What a window the kit opens leaves out, and what the window being drawn
// is: where, how big, whether the mouse or the keyboard is on it, how far
// it is scrolled. A window has everything unless it says otherwise:
//
//     ui::begin_host_window("##Hub", {.scrollbar = false, .to_front = false});
//
//     if (ui::window_focused() && ui::key_pressed(ui::Key::Delete)) remove_selected();

#include <glm/vec2.hpp>

namespace ftk::ui {

struct WindowOptions {
    bool title_bar{true};
    bool resize{true};
    bool move{true};
    bool collapse{true};
    bool scrollbar{true};
    bool background{true};
    /// Can be docked into a dockspace.
    bool docking{true};
    /// Comes in front of the windows over it when clicked. A host stays
    /// behind them.
    bool to_front{true};
    /// Ctrl Tab and the keyboard's moves between windows reach it.
    bool nav_focus{true};
    /// Where it was left and its size are saved with the layout and come
    /// back next run.
    bool remembered{true};
};

/// The next window opens at `pos`, `size` big, and stays there however it is
/// dragged: a host filling its place.
void next_window_place(glm::vec2 pos, glm::vec2 size);

/// The next window's size the first time it opens with no layout saved;
/// after that it keeps the size it is left at.
void next_window_size_once(glm::vec2 size);

/// The window being drawn: its top-left on screen and its size, its title
/// bar and padding included.
[[nodiscard]] glm::vec2 window_pos();
[[nodiscard]] glm::vec2 window_size();

/// Where its content goes, on screen: inside the padding, as scrolled now.
[[nodiscard]] glm::vec2 window_content_min();
[[nodiscard]] glm::vec2 window_content_max();

/// The mouse is over the window being drawn, and nothing above it has the
/// mouse; or over it or a window inside it.
[[nodiscard]] bool window_hovered();
[[nodiscard]] bool window_or_child_hovered();

/// The window being drawn has the keyboard; or it or a window inside it.
[[nodiscard]] bool window_focused();
[[nodiscard]] bool window_or_child_focused();

/// How far the window being drawn is scrolled down, and how far it can be.
[[nodiscard]] float scroll_y();
[[nodiscard]] float scroll_max_y();
void set_scroll_y(float y);

/// Scrolls the window being drawn so the cursor's line sits `ratio` of the
/// way down it: 0 the top, 1 the bottom, as a log follows its last line.
void scroll_to_cursor(float ratio);

namespace detail {

/// ImGui's window flags for `options`, while ImGui draws the kit.
[[nodiscard]] int imgui_window_flags(const WindowOptions& options);

} // namespace detail

} // namespace ftk::ui
