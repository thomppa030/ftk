#pragma once

// The order a program's loop over its window takes each turn in: the events
// the platform sent, the program's own update, whether the window is to
// close, a frame, then sleep until there is a reason for the next. A close
// asked for while the loop slept reaches the program's update before the
// loop looks at it, so the program can cancel it and ask about unsaved work
// first. AppWindow::run() is this loop over its window and frame; a test
// runs it over steps of its own.

#include "ftk/base/result.hpp"

#include <functional>

namespace ftk {

/// What run_window_loop() does in each turn, in this order.
struct WindowLoopSteps {
    /// Takes what the platform sent since the last turn, without waiting.
    std::function<void()> take_events{};
    /// The program's own work between frames, which may cancel a close the
    /// window was asked for. None does nothing.
    std::function<void()> update{};
    /// The window was asked to close, and the update didn't cancel it.
    std::function<bool()> closing{};
    /// Draws a frame, where there is room to.
    /// @return nothing, or why the frame could not be shown
    std::function<Result<>()> draw{};
    /// Sleeps until there is input, or until what the last frame waits on.
    std::function<void()> wait{};
};

/// Runs turns of `steps` until the window is to close once the update has
/// run, or a frame cannot be shown.
/// @return nothing, or why a frame could not be shown
[[nodiscard]] Result<> run_window_loop(const WindowLoopSteps& steps);

} // namespace ftk
