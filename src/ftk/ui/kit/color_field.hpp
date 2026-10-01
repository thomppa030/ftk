#pragma once

#include "ftk/ui/kit/edit.hpp"

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

} // namespace ftk::ui
