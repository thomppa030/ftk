#pragma once

#include "ftk/math/curve.hpp"
#include "ftk/ui/kit/edit.hpp"
#include "ftk/ui/theme.hpp"

#include <glm/vec2.hpp>
#include <imgui.h>

// A curve over time 0..1 on a canvas (sheet 8): its keys as handles, the
// selected key's tangents to drag, the curve in its track's colour.
//
//     edit |= ui::curve_editor("##alpha", module.curve_a, {.category = theme::Category::Vfx});
//
// Double-click or the right-click menu adds a key, Del removes the
// selected one, the wheel zooms, a middle or Alt drag pans, F frames the
// curve. The first and last keys stay at 0 and 1; keys between keep their
// order. The view is kept per editor in ImGui's storage.

namespace ftk::ui {

struct CurveOptions {
    /// Width (-1 for all there is) and height, gutters included.
    ImVec2 size{-1.0f, 120.0f};
    float v_min{0.0f};
    float v_max{1.0f};
    /// The kind of thing the curve drives, which colours it.
    theme::Category category{theme::Category::Vfx};
};

/// Draws the editor. `changed` on every frame the curve moves, `committed`
/// once per finished edit: a drag let go, a key added or removed.
Edit curve_editor(const char* id, Curve& curve, const CurveOptions& options = {});

/// Where the point `at` (time, value) of the editor drawn under `id` in the
/// current ID scope is on screen, as last drawn.
[[nodiscard]] ImVec2 curve_editor_point(const char* id, glm::vec2 at);

} // namespace ftk::ui
