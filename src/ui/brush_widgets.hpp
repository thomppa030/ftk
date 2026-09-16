#pragma once

#include "ui/brush.hpp"

#include <imgui.h>

namespace fjell {

/// The shared brush sliders, for any inspector that offers a brush. A tool
/// draws its own rows — which layer to paint, which sculpt operation —
/// around these. Returns true when a value changed.
inline bool draw_brush_settings(BrushSettings& brush) {
    bool changed = false;
    if (ImGui::SliderFloat("Radius (m)", &brush.radius, BrushSettings::MIN_RADIUS,
                           BrushSettings::MAX_RADIUS, "%.1f", ImGuiSliderFlags_Logarithmic)) {
        changed = true;
    }
    if (ImGui::SliderFloat("Strength", &brush.strength, 0.0f, 1.0f, "%.2f")) changed = true;
    if (ImGui::SliderFloat("Hardness", &brush.hardness, 0.0f, 1.0f, "%.2f")) changed = true;
    ImGui::TextDisabled("Ctrl+wheel radius   +Shift strength   +Alt hardness");
    ImGui::SetItemTooltip("Or [ and ]: bare for the radius, Shift for the strength,\n"
                          "Ctrl for the hardness.");
    return changed;
}

} // namespace fjell
