#pragma once

#include "ftk/ui/kit/dialog.hpp"

#include <glm/vec2.hpp>

// A window floating over the editor that shows one thing and the few things
// to do about it (sheet 54: a save): never docked, first shown centred at
// its size, then where it was left. What it holds scrolls on its own between
// its title bar and a ruled bar of buttons along its foot. Its ×, the bar's
// cancel and Esc while it has the keyboard close it; Enter presses nothing,
// so no key pressed elsewhere in the editor answers for it.
//
//     if (auto window = ui::FloatingWindow("quick.fjsave###save", &open_, {560.0f, 720.0f})) {
//         draw_what_it_shows();
//         if (window.bar({.confirm = "Play from this save", .cancel = "Close"}) == ui::DialogAnswer::Confirm) {
//             play();
//         }
//     }

namespace ftk::ui {

class FloatingWindow {
public:
    /// `open` is cleared as it closes.
    FloatingWindow(const char* title, bool* open, glm::vec2 size);
    ~FloatingWindow();

    FloatingWindow(const FloatingWindow&) = delete;
    FloatingWindow& operator=(const FloatingWindow&) = delete;
    FloatingWindow(FloatingWindow&&) = delete;
    FloatingWindow& operator=(FloatingWindow&&) = delete;

    explicit operator bool() const { return open_; }

    /// Ends what it holds and draws the bar along its foot: a rule across
    /// the window, then the cancel and the verb at its right, the verb greyed
    /// out with `buttons.why_not` as its tooltip while it can't be pressed.
    /// Returns the answer on the frame it is given; a cancel has closed it.
    DialogAnswer bar(const DialogSpec& spec, const detail::DialogButtons& buttons = {});

private:
    void end_body();

    bool open_{false};
    bool body_{false};
    bool* open_flag_{nullptr};
};

} // namespace ftk::ui
