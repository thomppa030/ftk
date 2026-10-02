#pragma once

// A picture as an item: a texture the UI shows, at the size it takes.
//
//     if (const ui::Texture thumb = icons.thumbnail(path)) ui::image(thumb, {64.0f, 64.0f});

#include "ftk/ui/types.hpp"

#include <glm/vec2.hpp>

namespace ftk::ui {

/// `texture` filling `size` at the cursor.
void image(Texture texture, glm::vec2 size);

} // namespace ftk::ui
