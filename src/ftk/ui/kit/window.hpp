#pragma once

// What a window the kit opens leaves out. A window has everything unless
// it says otherwise:
//
//     ui::begin_host_window("##Hub", {.scrollbar = false, .to_front = false});

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

namespace detail {

/// ImGui's window flags for `options`, while ImGui draws the kit.
[[nodiscard]] int imgui_window_flags(const WindowOptions& options);

} // namespace detail

} // namespace ftk::ui
