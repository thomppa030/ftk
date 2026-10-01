#pragma once

#include <string_view>

// The strip along the bottom of the editor: what is being edited and in
// which mode on the left, how the editor is running on the right.
//
//     if (auto bar = ui::StatusBar()) {
//         if (ui::status_button(ui::icon::content_browser, "Content", "Ctrl Space", open)) toggle();
//         ui::status_rule();
//         ui::status_item(ui::icon::scene, "harbour", scene.is_dirty());
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

/// A thin rule between groups of items.
void status_rule();

/// How a pill reads: plain, or in a state's colour.
enum class StatusTone { Plain, Success, Warning };

/// A state that matters at a glance (Playing, Paused), in a small rounded
/// pill tinted by `tone`.
void status_pill(const char* icon, const char* text, StatusTone tone);

/// A button on the bar that opens something (the content browser): a ghost
/// button with its icon, label and, after it, its shortcut as a dim hint.
/// Washed in the selection colour while what it opens is `open`. Returns
/// true when clicked.
bool status_button(const char* icon, const char* label, const char* shortcut, bool open);

} // namespace ftk::ui
