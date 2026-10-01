#pragma once

#include <imgui.h>

// Fjell's own marks (sheet 9): the logo and the word, which the hub and its
// splash show. One function each, so everything that shows them draws the
// same thing.

namespace fjell {

/// The Fjell logo: a ridge of mountains over its reflection, the peak in the
/// accent. `top_centre` is the middle of its top edge.
void draw_logo(ImDrawList* draw_list, ImVec2 top_centre, float scale);
/// How large draw_logo() draws at `scale`.
[[nodiscard]] ImVec2 logo_size(float scale);

/// "FJELL" in the bold face, the letters spaced half a letter apart,
/// centred on `centre` at `size` pixels.
void wordmark(ImDrawList* draw_list, ImVec2 centre, float size);

} // namespace fjell
