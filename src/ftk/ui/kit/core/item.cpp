#include "ftk/ui/kit/item.hpp"

#include "ftk/ui/kit/core/kit.hpp"

namespace ftk::ui {

using namespace corekit;

bool item_hovered() {
    const Item& item = kit().item;
    if (item.takes) return item.in.hovered;
    return ui().pointer_over(item.rect) && !ui().any_held();
}

bool item_under_mouse() {
    const Item& item = kit().item;
    if (item.takes) return item.in.under;
    return ui().pointer_over(item.rect);
}

// An item the keyboard edits is active while it has the keyboard; another
// while a pointer holds it.

bool item_active() {
    const Item& item = kit().item;
    if (item.keyboard) return item.focused;
    if (!item.takes) return item.holds;
    return item.in.held || item.focused;
}

bool item_activated() {
    const Item& item = kit().item;
    if (item.keyboard) return item.focus_gained;
    return item.takes && (item.in.pressed || item.focus_gained);
}

bool item_deactivated() {
    const Item& item = kit().item;
    if (item.keyboard) return item.focus_lost;
    return item.takes && (item.in.released || item.focus_lost);
}

bool item_deactivated_after_edit() {
    return item_deactivated() && kit().item.edited;
}

bool item_clicked(MouseButton button) {
    const Item& item = kit().item;
    // A press the item takes, or one with a button it doesn't take while
    // the mouse is on it, as a right click on a button opens its menu.
    if (item.takes && item.in.pressed && item.in.button == button) return true;
    return item_hovered() && mouse_clicked(button);
}

glm::vec2 item_min() {
    return kit().item.rect.min;
}

glm::vec2 item_max() {
    return kit().item.rect.max;
}

bool any_item_active() {
    return ui().any_held() || ui().typing();
}

bool any_item_hovered() {
    return ui().any_hovered();
}

void next_item_allows_overlap() {
    kit().next.allow_overlap = true;
}

bool hit_area(const char* id, glm::vec2 size, HitButtons buttons) {
    const core::Rect rect = ui().layout().next(size);
    return place(ui().id(id), rect, {.left = buttons.left, .right = buttons.right, .middle = buttons.middle}).clicked;
}

} // namespace ftk::ui
