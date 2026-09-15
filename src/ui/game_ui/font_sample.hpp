#pragma once

#include <string>

namespace fjell {

class GameUi;

/// Loads `ui` with one line of text drawn in the font of `recipe_path`,
/// through a sheet built in memory that names the recipe with @font-face.
/// What a font's thumbnail and the font editor's preview both show. The
/// atlas has to be in the font library already; until it is, the line draws
/// in the default font.
void load_font_sample(GameUi& ui, const std::string& recipe_path, const std::string& text,
                      float font_size);

} // namespace fjell
