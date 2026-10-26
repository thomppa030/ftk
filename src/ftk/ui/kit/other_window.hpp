#pragma once

// A window of the program's own beside the one being drawn, which the program
// fills: an editor's tab in a window of its own. What is drawn while the scope
// lives goes into that window, laid out at its size, with bars, a dockspace
// and windows of its own, as into the program's main window. It has no parent:
// a tiling window manager tiles it, the taskbar shows it, and it goes behind
// the main window or onto another screen as its user puts it.
//
//     const ui::OtherWindow torn("##torn bark.fjmat", {.title = "bark.fjmat"});
//     if (torn) draw_editor();
//     if (torn.close_asked()) asking_ = true;          // unsaved work asked about there
//     ...
//     ui::close_other_window("##torn bark.fjmat");   // the program closes it

#include "ftk/ui/core/input.hpp"

#include <glm/vec2.hpp>

#include <string>

namespace ftk::ui {

struct OtherWindowSpec {
    /// Its title, as the window manager and the taskbar show it.
    std::string title{};
    /// Its size as it opens, in the layout units of the window being drawn.
    glm::vec2 size{960.0f, 640.0f};
    /// Where it opens, in the same units from that window's top left corner,
    /// as near as the platform lets a program place a window.
    glm::vec2 at{0.0f};
};

/// The window `id` while it lives: asked of the program the first frame it is
/// drawn, and drawn into from the frame after, once the program has opened
/// it. Its title follows `spec`; its size and place are its user's once open.
class OtherWindow {
public:
    OtherWindow(const char* id, const OtherWindowSpec& spec);
    ~OtherWindow();

    OtherWindow(const OtherWindow&) = delete;
    OtherWindow& operator=(const OtherWindow&) = delete;
    OtherWindow(OtherWindow&&) = delete;
    OtherWindow& operator=(OtherWindow&&) = delete;

    /// It is open and what follows is drawn into it.
    explicit operator bool() const { return open_; }

    /// Its user asked to close it this frame: it stays open until the
    /// program closes it (close_other_window()), or keeps it.
    [[nodiscard]] bool close_asked() const { return close_asked_; }

    /// The platform closed it without asking: it is gone, and drawn again it
    /// opens anew.
    [[nodiscard]] bool closed() const { return closed_; }

    /// Brings it in front of the program's other windows with the keyboard,
    /// where the platform lets a program (gpu Window::raise()).
    void raise();

private:
    core::RootId root_{0};
    bool open_{false};
    bool closed_{false};
    bool close_asked_{false};
};

/// Closes the window `id` an OtherWindow drew: the program takes back what
/// it showed there. A window not open is left as it is.
void close_other_window(const char* id);

/// The OS window an OtherWindow `id` draws into, as the frame's requests and
/// the program's windows know it (UiLayer::on_window_opened).
[[nodiscard]] core::RootId other_window_root(const char* id);

} // namespace ftk::ui
