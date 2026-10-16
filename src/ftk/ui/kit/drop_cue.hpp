#pragma once

// Where files dragged in from outside the program will go (sheet 46, G3):
// the area that takes them outlined in amber dashes over a wash, and a plate
// on the accent across its bar naming the folder. Drawn over what they cover
// while ui::dragging_from_outside().
//
//     if (ui::dragging_from_outside()) {
//         ui::drop_rim(grid_min, grid_max);
//         ui::drop_plate(bar_min, bar_max, "assets/props");
//     }

#include <glm/vec2.hpp>

#include <string_view>

namespace ftk::ui {

/// The area from `min` to `max` outlined in amber dashes over a wash.
void drop_rim(glm::vec2 min, glm::vec2 max);

/// A plate on the accent in the middle of the bar from `min` to `max`: the
/// import arrow, "Drop to copy into", and `where` in the mono face.
void drop_plate(glm::vec2 min, glm::vec2 max, std::string_view where);

} // namespace ftk::ui
