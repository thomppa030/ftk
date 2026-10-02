#pragma once

// Entries of a menu or popup (sheet 6): an icon column, kept when an entry
// has no icon, the label, and the real shortcut on the right in the mono
// face. A destructive entry is drawn in the error colour and goes last.

#include <optional>

namespace ftk::ui {

struct MenuItem {
    const char* icon{nullptr};      ///< from ui::icon; null leaves the column empty
    const char* label{""};          ///< a verb in sentence case: "Fit to mesh"
    const char* shortcut{nullptr};  ///< "Ctrl D", when the action has one
    bool enabled{true};
    const char* disabled_reason{nullptr};  ///< the tooltip while disabled
    bool destructive{false};
    /// A setting that is on or off ("Show grid"): a check in the icon
    /// column while on, in place of an icon.
    std::optional<bool> checked{};
};

/// One entry. Returns true when chosen; the menu then closes.
bool menu_item(const MenuItem& item);

/// A heading over a group of entries ("Lighting", "Buffers"): uppercase,
/// small and dim, lined up with the entries' labels. Not an entry.
void menu_heading(const char* label);

/// An entry that opens a submenu ("Create new"), led by its icon. Returns
/// true while the submenu is open; the caller then draws its entries and
/// calls end_menu().
bool begin_menu(const char* icon, const char* label, bool enabled = true);

/// Ends a submenu begin_menu() or a menu begin_bar_menu() opened.
void end_menu();

/// The bar of menus across the top of the program's window. Its menus go
/// inside the scope while this is true.
///
///     if (auto bar = ui::MenuBar()) {
///         if (ui::begin_bar_menu("File")) {
///             if (ui::menu_item({.label = "Save scene", .shortcut = "Ctrl S"})) save();
///             ui::end_menu();
///         }
///     }
class MenuBar {
public:
    MenuBar();
    ~MenuBar();
    MenuBar(const MenuBar&) = delete;
    MenuBar& operator=(const MenuBar&) = delete;
    MenuBar(MenuBar&&) = delete;
    MenuBar& operator=(MenuBar&&) = delete;

    explicit operator bool() const { return open_; }

private:
    bool open_{false};
};

/// A menu on the bar, by its name ("File"). Returns true while it is open;
/// the caller then draws its entries and calls end_menu().
bool begin_bar_menu(const char* label);

} // namespace ftk::ui
