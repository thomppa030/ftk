#pragma once

#include <string>

namespace fjell {

class FontAtlas;
class GameUi;

/// Loads `ui` with one line of text drawn in the font of `recipe_path`,
/// through a sheet built in memory that names the recipe with @font-face.
/// What a font's thumbnail and the font editor's preview both show. The
/// line sits in a box measured with `atlas`, the recipe's own atlas from
/// the font library: a text node has no box of its own in layout, and the
/// thumbnail crops to boxes. Without an atlas the box is a guess and the
/// line draws in the default font.
void load_font_sample(GameUi& ui, const std::string& recipe_path, const std::string& text,
                      float font_size, const FontAtlas* atlas);

} // namespace fjell
