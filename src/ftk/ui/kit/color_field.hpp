#pragma once

#include "ftk/ui/kit/edit.hpp"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace ftk::ui {

/// What a colour value means to the renderer. The field always shows and
/// takes sRGB, the same as the scene file and every picker; a linear value
/// is converted on the way in and out.
enum class ColorSpace {
    Srgb,    ///< stored as authored: UI tints, swatches, anything not lit
    Linear,  ///< multiplied as light: light colours, emission, fog
};

/// A colour: a swatch that opens the picker, then its hex code, which can
/// be typed ("#D4A054"). A colour with alpha shows eight digits. Each
/// component stays within 0–1; brightness above that is a separate
/// Intensity field.
Edit color(const char* id, glm::vec3& value, ColorSpace space);
Edit color(const char* id, glm::vec4& value, ColorSpace space);

/// A square of `colour`, `size` big: one of a set of colours to choose.
/// Returns true when clicked.
bool swatch(const char* id, glm::vec4 colour, glm::vec2 size);

/// A swatch of `colour` (sRGB) drawn at `min`..`max` as a colour field
/// draws its own: edged in the sunken grey and, with `alpha`, its right half
/// over checks as the colour's alpha leaves it. For a swatch whose place
/// and clicks are the caller's, as a colour value's in code: it records
/// nothing and takes no input.
void draw_swatch(glm::vec2 min, glm::vec2 max, glm::vec4 colour, bool alpha);

/// The picker a colour field opens, in the popup `id`, which its caller
/// opens where it wants it (open_popup(id, at)): while it is open, the
/// square, the hue bar, with `alpha` the alpha bar, and the colour's
/// numbers, editing `srgb` as they are dragged or typed. It closes as a
/// popup does, on Esc or a click outside it. What changed this frame; a
/// closed one changes nothing.
Edit color_popup(const char* id, glm::vec4& srgb, bool alpha);

/// The picker itself, in place rather than behind a swatch, for a menu
/// that is about one colour: the square, the hue bar, and the hex code
/// under them, as wide as the next width. `value` is sRGB.
Edit color_picker(const char* id, glm::vec3& value);

} // namespace ftk::ui
