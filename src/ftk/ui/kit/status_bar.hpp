#pragma once

#include <string_view>

// The strip along the bottom of the editor: what is being edited and in
// which mode on the left, how the editor is running on the right.
//
//     if (auto bar = ui::StatusBar()) {
//         if (ui::status_button(ui::icon::folder, "Content", "Ctrl Space", open)) toggle();
//         ui::status_rule();
//         ui::status_item(ui::icon::file, "harbour", scene.is_dirty());
//         bar.right();
//         ui::status_item(nullptr, "6.94 ms");
//     }

namespace ftk::ui {

/// The bar, pinned to the bottom of the main viewport for as long as it
/// lives. Items after right() sit against the right edge.
class StatusBar {
public:
    StatusBar();
    ~StatusBar();

    StatusBar(const StatusBar&) = delete;
    StatusBar& operator=(const StatusBar&) = delete;
    StatusBar(StatusBar&&) = delete;
    StatusBar& operator=(StatusBar&&) = delete;

    explicit operator bool() const { return open_; }

    /// Starts the right-hand group.
    void right();

private:
    bool open_{false};
    float right_start_{-1.0f};
};

/// A piece of state: an icon (or none) and its text, with the unsaved dot
/// after it when `unsaved`.
void status_item(const char* icon, std::string_view text, bool unsaved = false);

/// A figure that changes as the program runs (the frame time), in a place as
/// wide as `widest` would be, its text against the right of it, so what sits
/// beside it holds still while it changes. A text wider than `widest` widens
/// it.
void status_figure(std::string_view text, std::string_view widest);

/// Work going on in the background ("Baking 37 textures"): a small spinner
/// where an item's icon would be, then `text`.
void status_busy(std::string_view text);

/// A message and where it is from: `text`, then `where` ("coin.cpp:26")
/// small in the mono face and the disabled colour.
void status_note(std::string_view text, std::string_view where);

/// A thin rule between groups of items.
void status_rule();

/// How a pill or a state reads: plain, in a state's colour, or as a mode
/// that is on (the toggle colour: Vim's insert mode) or that selects (the
/// accent: its visual mode).
enum class StatusTone { Plain, Success, Warning, Error, On, Selected };

/// A state that matters at a glance (Playing, Paused, a modal editor's
/// mode), in a small rounded pill tinted by `tone`: its icon, when it has
/// one, then its text.
void status_pill(const char* icon, const char* text, StatusTone tone);

/// Something the program runs and how it is (a language server: running,
/// starting, stopped): a dot in `tone`'s colour, then its `name`. Hovered,
/// `tip` says more.
void status_state(StatusTone tone, std::string_view name, std::string_view tip = {});

/// What is broken (sheet 15): the errors' icon and count in the error
/// colour, the warnings' in the warning colour, then the newest problem's
/// sentence and, small in the mono face, where it is. All one button, which
/// opens the list of them; nothing when there are none. Returns true when
/// clicked.
bool status_problems(int errors, int warnings, std::string_view newest, std::string_view where);

/// A button on the bar that opens something (the content browser): a ghost
/// button with its icon, label and, after it, its shortcut as a dim hint.
/// Washed in the selection colour while what it opens is `open`. Returns
/// true when clicked.
bool status_button(const char* icon, const char* label, const char* shortcut, bool open);

} // namespace ftk::ui
