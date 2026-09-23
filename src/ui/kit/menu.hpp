#pragma once

// Entries of a menu or popup (sheet 6): an icon column, kept when an entry
// has no icon, the label, and the real shortcut on the right in the mono
// face. A destructive entry is drawn in the error colour and goes last.

namespace fjell::ui {

struct MenuItem {
    const char* icon{nullptr};      ///< from ui::icon; null leaves the column empty
    const char* label{""};          ///< a verb in sentence case: "Fit to mesh"
    const char* shortcut{nullptr};  ///< "Ctrl D", when the action has one
    bool enabled{true};
    const char* disabled_reason{nullptr};  ///< the tooltip while disabled
    bool destructive{false};
};

/// One entry. Returns true when chosen; the menu then closes.
bool menu_item(const MenuItem& item);

} // namespace fjell::ui
