#pragma once

// What a window the kit opens leaves out, and what the window being drawn
// is: where, how big, whether the mouse or the keyboard is on it, how far
// it is scrolled. A window has everything unless it says otherwise:
//
//     ui::begin_host_window("##Hub", {.scrollbar = false, .to_front = false});
//
//     if (ui::window_focused() && ui::key_pressed(ui::Key::Delete)) remove_selected();

#include <glm/vec2.hpp>

#include <optional>
#include <string>
#include <vector>

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

/// The next window goes at `pos` this frame, wherever it was left: a
/// panel pinned to an edge.
void next_window_pos(glm::vec2 pos);

/// The next window can be resized only between `min` and `max`.
void next_window_size_limits(glm::vec2 min, glm::vec2 max);

/// The part of the program's window that panels go in, less its menu bar,
/// in screen pixels.
struct WorkArea {
    glm::vec2 pos;
    glm::vec2 size;
};
[[nodiscard]] WorkArea work_area();

/// Pixels to one of the UI's units in the OS window being drawn: its
/// display's density times the scale chosen for content there.
[[nodiscard]] float pixels_per_unit();

/// A window of the program's own, as it was last drawn: its name (with any
/// "##" or "###" part) and where it is on screen.
struct ShownWindow {
    std::string name;
    glm::vec2 pos;
    glm::vec2 size;
};

/// The top-level windows shown now, panels and dialogs, not the windows
/// inside them: to pick one, such as a pane to capture.
[[nodiscard]] std::vector<ShownWindow> shown_windows();

/// The window `name`, when there is one.
[[nodiscard]] std::optional<ShownWindow> window_named(const std::string& name);

/// While it lives, what is drawn goes into the window `name`, drawn earlier
/// this frame, over what it already holds: an overlay on a viewport drawn
/// by someone else. Draw while it is true, which is while the window shows.
class InWindow {
public:
    explicit InWindow(const char* name);
    ~InWindow();
    InWindow(const InWindow&) = delete;
    InWindow& operator=(const InWindow&) = delete;
    InWindow(InWindow&&) = delete;
    InWindow& operator=(InWindow&&) = delete;

    explicit operator bool() const { return visible_; }

private:
    bool visible_{false};
};

/// Gives the window `name` the keyboard and brings it to the front, as a
/// click on it would.
void focus_window(const char* name);

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
/// The mouse is over the window being drawn, even while an item is held:
/// a row that stays lit while its own button is pressed.
[[nodiscard]] bool window_under_mouse();

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

} // namespace ftk::ui
