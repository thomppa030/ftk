#pragma once

#include "renderer/systems/vfx_curve.hpp"

#include <imgui.h>

namespace fjell {

/// Interactive curve editor widget. Returns true if the curve was modified.
/// @param label   Unique ImGui ID
/// @param curve   The curve to edit (modified in place)
/// @param size    Widget size (default: full width, 150px height)
/// @param v_min   Minimum value axis
/// @param v_max   Maximum value axis
bool CurveEditor(const char* label, Curve& curve, ImVec2 size = {-1, 150},
                 float v_min = 0.0f, float v_max = 1.0f);

} // namespace fjell
