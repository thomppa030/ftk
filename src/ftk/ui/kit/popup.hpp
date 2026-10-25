#pragma once

// Popups (sheet 6): a menu or a picker over everything else, opened by name
// and closed by choosing, Esc or a click outside it. Its entries are the
// kit's menu items.
//
//     if (ui::item_clicked(ui::MouseButton::Right)) ui::open_popup("##track");
//     if (auto menu = ui::Popup("##track")) {
//         if (ui::menu_item({.label = "Delete track", .destructive = true})) remove();
//     }
//
//     if (auto menu = ui::ContextMenu("##row")) { ... }  // right-click on the last item

#include <glm/vec2.hpp>

namespace ftk::ui {

/// Opens the popup `id` at the mouse, to be drawn from the next Popup of
/// that id. `id` is under the ids pushed around the call.
void open_popup(const char* id);
/// The same at `at` in the window's units: where something happened a frame
/// or two before the popup could open (a click whose pick view answered
/// since), wherever the mouse is now.
void open_popup(const char* id, glm::vec2 at);

/// Whether the popup `id` is open.
[[nodiscard]] bool popup_open(const char* id);

/// Whether any popup is open: a menu, a picker or a dialog.
[[nodiscard]] bool any_popup_open();

/// Closes the popup being drawn: one of its entries was chosen.
void close_popup();

/// The popup `id` while it is open: its entries go inside the scope while
/// this is true.
class Popup {
public:
    explicit Popup(const char* id);
    ~Popup();
    Popup(const Popup&) = delete;
    Popup& operator=(const Popup&) = delete;
    Popup(Popup&&) = delete;
    Popup& operator=(Popup&&) = delete;

    explicit operator bool() const { return open_; }

private:
    bool open_{false};
};

/// The popup of the item just drawn, opened by a right click on it. Each
/// item has its own: the rows of a list may all name theirs `id`.
class ContextMenu {
public:
    explicit ContextMenu(const char* id);
    ~ContextMenu();
    ContextMenu(const ContextMenu&) = delete;
    ContextMenu& operator=(const ContextMenu&) = delete;
    ContextMenu(ContextMenu&&) = delete;
    ContextMenu& operator=(ContextMenu&&) = delete;

    explicit operator bool() const { return open_; }

private:
    bool open_{false};
};

} // namespace ftk::ui
