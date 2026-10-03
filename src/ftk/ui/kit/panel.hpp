#pragma once

#include "ftk/ui/kit/window.hpp"

namespace ftk::ui {

/// Custom-titled panel: a window with a header of its own.
///
///     if (auto p = ui::Panel(ui::icon::history, "History")) {
///         ...
///     }
///
/// Hides the default title bar and draws a 28px header strip with:
///   - leading icon glyph (optional, pass nullptr to skip)
///   - bold label
///   - dimmed underline for separation from content
///
/// The window still docks (a dockspace needs no title bar) and the tab in
/// the dockspace shows the label passed to the constructor (used as the
/// window name). It never collapses.
class Panel {
public:
    Panel(const char* icon, const char* label, WindowOptions options = {})
        : Panel(icon, label, nullptr, options) {}

    /// Closable variant: pass a bool* for the close-button state.
    Panel(const char* icon, const char* label, bool* p_open, WindowOptions options = {});

    ~Panel();

    Panel(const Panel&) = delete;
    Panel& operator=(const Panel&) = delete;
    Panel(Panel&&) = delete;
    Panel& operator=(Panel&&) = delete;

    explicit operator bool() const { return open_; }

private:
    bool open_{false};
};

} // namespace ftk::ui
