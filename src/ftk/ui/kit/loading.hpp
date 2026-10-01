#pragma once

#include <imgui.h>

#include <string_view>

// What a window shows while something loads (sheet 9): a turning arc, and the
// arc with a caption saying what.

namespace ftk::ui {

/// Something is loading: an arc in the accent turning around `centre`,
/// `seconds` since it started.
void spinner(ImDrawList* draw_list, ImVec2 centre, float radius, float seconds);

/// Something is loading and this is what: the spinner with its caption
/// under it in the secondary colour ("Opening harbour…"), centred on
/// `top_centre`, the middle of the spinner's top edge.
void loading(ImDrawList* draw_list, ImVec2 top_centre, std::string_view what, float seconds);

} // namespace ftk::ui
