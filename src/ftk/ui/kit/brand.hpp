#pragma once

#include <imgui.h>

#include <string_view>

// Fjell's own marks (sheet 9): the logo, the word, and what the hub and the
// splash show while something loads. One function each, so the hub, the
// splash and anything after them draw the same thing.

namespace ftk::ui {

/// The Fjell logo: a ridge of mountains over its reflection, the peak in the
/// accent. `top_centre` is the middle of its top edge.
void draw_logo(ImDrawList* draw_list, ImVec2 top_centre, float scale);
/// How large draw_logo() draws at `scale`.
[[nodiscard]] ImVec2 logo_size(float scale);

/// "FJELL" in the bold face, the letters spaced half a letter apart,
/// centred on `centre` at `size` pixels.
void wordmark(ImDrawList* draw_list, ImVec2 centre, float size);

/// Something is loading: an arc in the accent turning around `centre`,
/// `seconds` since it started.
void spinner(ImDrawList* draw_list, ImVec2 centre, float radius, float seconds);

/// Something is loading and this is what: the spinner with its caption
/// under it in the secondary colour ("Opening harbour…"), centred on
/// `top_centre`, the middle of the spinner's top edge.
void loading(ImDrawList* draw_list, ImVec2 top_centre, std::string_view what, float seconds);

} // namespace ftk::ui
