#pragma once

// A box dragged out to select what lies inside it: the accent's outline over
// an 8 % wash of it, as a node graph and the Scene viewport draw one. Drawn
// into the current window between the corners the drag spans, in either
// order.
//
//     if (dragging) ui::selection_box(pressed_at, mouse_pos());

#include <glm/vec2.hpp>

namespace ftk::ui {

/// The box from `corner` to `opposite`, the corners in any order.
void selection_box(glm::vec2 corner, glm::vec2 opposite);

} // namespace ftk::ui
