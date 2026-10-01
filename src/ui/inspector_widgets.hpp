#pragma once

#include "ftk/math/color_space.hpp"
#include "ftk/ui/kit/button.hpp"

#include <glm/glm.hpp>
#include <imgui.h>

namespace fjell {

/// Colour picker for a value the renderer multiplies as linear light. The
/// swatch and the numbers are sRGB, the same as the scene file, so what is
/// picked is what the light or sky looks like. Returns true on edit.
inline bool color_edit_linear(const char* id, glm::vec3& linear) {
    glm::vec3 srgb = ftk::color_space::linear_to_srgb(linear);
    if (!ImGui::ColorEdit3(id, &srgb.x)) return false;
    linear = ftk::color_space::srgb_to_linear(srgb);
    return true;
}

/// One toggle in a row of tools. Kept for the callers written before the
/// kit; new code uses ui::toggle_button (icon) or ui::toggle (text).
inline bool toggle_button(const char* id, const char* label, bool on, const char* tooltip) {
    ImGui::PushID(id);
    const bool clicked = ftk::ui::toggle(label, on, tooltip);
    ImGui::PopID();
    return clicked;
}

} // namespace fjell
