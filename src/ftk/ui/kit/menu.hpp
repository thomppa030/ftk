#pragma once

// Entries of a menu or popup (sheet 6): an icon column, kept when an entry
// has no icon, the label, and the real shortcut on the right in the mono
// face. A destructive entry is drawn in the error colour and goes last.
//
// Menus are data, drawn in the window everywhere (decision 21 of the UI
// core plan), so a native menu bar can show the same ones later:
//
//     const ui::Menu file{"File", [&] {
//         return std::vector<ui::MenuEntry>{
//             {.label = "Save scene", .icon = icon::save, .shortcut = "Ctrl S", .chosen = [&] { save(); }},
//             ui::MenuEntry::separator(),
//             {.label = "Show grid", .checked = grid.visible, .chosen = [&] { grid.visible = !grid.visible; }},
//         };
//     }};
//     ui::menu_bar(std::array{file});

#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/theme.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ftk::ui {

struct MenuItem {
    const char* icon{nullptr};      ///< from ui::icon; null leaves the column empty
    /// The icon's colour where it stands for a kind with a hue of its own (a
    /// file's kind, as the content browser colours it); the label's when unset.
    std::optional<Color> icon_colour{};
    const char* label{""};          ///< a verb in sentence case: "Fit to mesh"
    /// What the entry is to what the menu acts on ("Model", a slot's name),
    /// after the label in the secondary text colour; null for none.
    const char* detail{nullptr};
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

/// What the entries under it act on, named as an entry is laid out (the node
/// a viewport's menu was opened on): its icon and kind in the secondary text
/// colour, its name as written in bold. Not an entry.
void menu_title(const char* icon, std::string_view name, std::string_view kind);

/// An entry that opens a submenu ("Create new"), led by its icon, as tall as
/// any other entry. Returns true while the submenu is open; the caller then
/// draws its entries and calls end_menu().
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

/// An entry of a menu as data.
struct MenuEntry {
    enum class Kind : uint8_t {
        /// Chosen, its `chosen` runs and the menu closes.
        Action,
        /// A line between groups of entries.
        Separator,
        /// A heading over a group of entries: not an entry.
        Heading,
        /// A line of dim text that tells something ("No project open"): not
        /// an entry.
        Note,
        /// An entry that opens `entries` as a submenu.
        Submenu,
        /// What the program draws itself with `draw`, in the window's menu
        /// only: fields set in place. A native menu passes it over.
        Custom,
    };
    Kind kind{Kind::Action};
    std::string label{};
    const char* icon{nullptr};
    std::optional<Color> icon_colour{};
    std::string detail{};
    std::string shortcut{};
    bool enabled{true};
    std::string disabled_reason{};
    bool destructive{false};
    std::optional<bool> checked{};
    std::function<void()> chosen{};
    std::vector<MenuEntry> entries{};
    std::function<void()> draw{};

    static MenuEntry separator() { return {.kind = Kind::Separator}; }
    static MenuEntry heading(std::string label) { return {.kind = Kind::Heading, .label = std::move(label)}; }
    static MenuEntry note(std::string text) { return {.kind = Kind::Note, .label = std::move(text)}; }
    static MenuEntry submenu(const char* icon, std::string label, std::vector<MenuEntry> entries) {
        return {.kind = Kind::Submenu, .label = std::move(label), .icon = icon, .entries = std::move(entries)};
    }
    static MenuEntry custom(std::function<void()> draw) { return {.kind = Kind::Custom, .draw = std::move(draw)}; }
};

/// A menu of the bar as data: its name, and its entries, made as it opens,
/// as what they show changes from one opening to the next.
struct Menu {
    std::string label;
    std::function<std::vector<MenuEntry>()> entries;
};

/// `entries` in the menu or popup being drawn. An entry chosen runs its
/// `chosen` there, inside the menu.
inline void menu_entries(std::span<const MenuEntry> entries) {
    for (const MenuEntry& entry : entries) {
        switch (entry.kind) {
        case MenuEntry::Kind::Action:
            if (menu_item({.icon = entry.icon,
                           .icon_colour = entry.icon_colour,
                           .label = entry.label.c_str(),
                           .detail = entry.detail.empty() ? nullptr : entry.detail.c_str(),
                           .shortcut = entry.shortcut.empty() ? nullptr : entry.shortcut.c_str(),
                           .enabled = entry.enabled,
                           .disabled_reason = entry.disabled_reason.empty() ? nullptr : entry.disabled_reason.c_str(),
                           .destructive = entry.destructive,
                           .checked = entry.checked}) &&
                entry.chosen) {
                entry.chosen();
            }
            break;
        case MenuEntry::Kind::Separator: separator(); break;
        case MenuEntry::Kind::Heading: menu_heading(entry.label.c_str()); break;
        case MenuEntry::Kind::Note: text(entry.label, theme::text_disabled()); break;
        case MenuEntry::Kind::Submenu:
            if (begin_menu(entry.icon, entry.label.c_str(), entry.enabled)) {
                menu_entries(entry.entries);
                end_menu();
            }
            break;
        case MenuEntry::Kind::Custom:
            if (entry.draw) entry.draw();
            break;
        }
    }
}

/// The bar of menus across the top of the program's window, drawn from
/// `menus`.
inline void menu_bar(std::span<const Menu> menus) {
    if (auto bar = MenuBar()) {
        for (const Menu& menu : menus) {
            if (!begin_bar_menu(menu.label.c_str())) continue;
            if (menu.entries) menu_entries(menu.entries());
            end_menu();
        }
    }
}

} // namespace ftk::ui
