#pragma once

// The item just drawn: whether the mouse is on it, whether it is being
// pressed or edited, where it is. And an item with nothing drawn, for a
// canvas or a row drawn by hand to take the mouse.
//
//     if (ui::hit_area("##lane", {width, LANE_H}, {.right = true})) select(lane);
//     if (ui::item_clicked(ui::MouseButton::Right)) open_menu(lane);

#include "ftk/ui/kit/input.hpp"

#include <glm/vec2.hpp>

namespace ftk::ui {

/// The mouse is on the item, and nothing else has it.
[[nodiscard]] bool item_hovered();
/// The item is being pressed, dragged or typed into.
[[nodiscard]] bool item_active();
/// It became so this frame.
[[nodiscard]] bool item_activated();
/// It stopped being so this frame.
[[nodiscard]] bool item_deactivated();
/// The same, after its value changed: an edit finished.
[[nodiscard]] bool item_deactivated_after_edit();
/// `button` went down on the item this frame.
[[nodiscard]] bool item_clicked(MouseButton button = MouseButton::Left);
/// The item's corners, in screen pixels.
[[nodiscard]] glm::vec2 item_min();
[[nodiscard]] glm::vec2 item_max();

/// Some item anywhere is being pressed, dragged or typed into.
[[nodiscard]] bool any_item_active();
/// The mouse is on some item anywhere.
[[nodiscard]] bool any_item_hovered();

/// The mouse buttons a hit area answers to.
struct HitButtons {
    bool left{true};
    bool right{false};
    bool middle{false};
};

/// An item `size` big at the cursor that draws nothing and takes the mouse.
/// Returns true when one of `buttons` clicks it.
bool hit_area(const char* id, glm::vec2 size, HitButtons buttons = {});

} // namespace ftk::ui
