#pragma once

// The kit's own types where its headers took ImGui's, so a program reaches
// the kit without ImGui. Points and sizes are glm::vec2.

#include <glm/vec4.hpp>

#include <cstdint>

namespace ftk::ui {

/// A colour: red, green, blue and alpha from 0 to 1, in sRGB like every
/// colour the theme names.
using Color = glm::vec4;

/// A widget's id: its label hashed with the ids pushed around it. While
/// ImGui draws the kit it is ImGui's own id.
using Id = uint32_t;

} // namespace ftk::ui
