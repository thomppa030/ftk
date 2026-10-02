#pragma once

#include "ftk/ui/draw_list.hpp"

#include <glm/vec2.hpp>

#include <string_view>

// What a window shows while something loads (sheet 9): a turning arc, and the
// arc with a caption saying what; or, when how far it has got is known, a
// bar.

namespace ftk::ui {

/// Something is loading: an arc in the accent turning around `centre`,
/// `seconds` since it started.
void spinner(DrawList draw_list, glm::vec2 centre, float radius, float seconds);

/// Something is loading and this is what: the spinner with its caption
/// under it in the secondary colour ("Opening harbour…"), centred on
/// `top_centre`, the middle of the spinner's top edge.
void loading(DrawList draw_list, glm::vec2 top_centre, std::string_view what, float seconds);

/// How far something has got, `fraction` of the way, as a bar `width` wide
/// (less than zero leaves that much room at the right) with `label` on it.
void progress_bar(float fraction, float width, const char* label);

} // namespace ftk::ui
