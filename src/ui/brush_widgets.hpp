#pragma once

#include "ftk/ui/kit/field.hpp"
#include "ftk/ui/kit/row.hpp"
#include "ui/brush.hpp"

namespace fjell {

/// The shared brush rows, for any inspector that offers a brush, drawn into
/// the caller's property table. A tool draws its own rows (which layer to
/// paint, which sculpt operation) around these. Brush settings are the
/// tool's, not the scene's, so changing them is no edit of the scene.
inline void draw_brush_settings(BrushSettings& brush) {
    ftk::ui::row("Radius", "Ctrl and the wheel, or [ and ], change it in the viewport.", [&] {
        (void)ftk::ui::slider("##radius", brush.radius, BrushSettings::MIN_RADIUS, BrushSettings::MAX_RADIUS,
                         ftk::ui::Unit::Metres, "%.1f", true);
    });
    ftk::ui::row("Strength", "Ctrl Shift and the wheel, or Shift [ and ], change it in the viewport.", [&] {
        (void)ftk::ui::slider("##strength", brush.strength, 0.0f, 1.0f);
    });
    ftk::ui::row("Hardness", "How much of the radius works at full strength before it fades. "
                        "Ctrl Alt and the wheel, or Ctrl [ and ], change it in the viewport.",
            [&] { (void)ftk::ui::slider("##hardness", brush.hardness, 0.0f, 1.0f); });
}

} // namespace fjell
