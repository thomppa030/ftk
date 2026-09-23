#pragma once

#include "core/math/color_space.hpp"
#include "ui/kit/button.hpp"
#include "ui/kit/row.hpp"
#include "ui/kit/section.hpp"
#include "ui/theme.hpp"

#include <glm/glm.hpp>
#include <imgui.h>
#include <imgui_internal.h>

#include <cstring>
#include <string>
#include <utility>

namespace fjell {

/// Colour picker for a value the renderer multiplies as linear light. The
/// swatch and the numbers are sRGB, the same as the scene file, so what is
/// picked is what the light or sky looks like. Returns true on edit.
inline bool color_edit_linear(const char* id, glm::vec3& linear) {
    glm::vec3 srgb = color_space::linear_to_srgb(linear);
    if (!ImGui::ColorEdit3(id, &srgb.x)) return false;
    linear = color_space::srgb_to_linear(srgb);
    return true;
}

/// One toggle in a row of tools. Kept for the callers written before the
/// kit; new code uses ui::toggle_button (icon) or ui::toggle (text).
inline bool toggle_button(const char* id, const char* label, bool on, const char* tooltip) {
    ImGui::PushID(id);
    const bool clicked = ui::toggle(label, on, tooltip);
    ImGui::PopID();
    return clicked;
}

/// A section heading. Kept for the callers written before the kit; new
/// code uses ui::section.
inline void section_header(const char* label, const char* icon = nullptr) {
    ui::section(label, icon);
}

/// The kit's property table and row, under the names written before the
/// kit; new code uses ui::PropertyTable and ui::row (ui/kit/row.hpp).
using PropertyTable = ui::PropertyTable;

template <typename F>
inline void prop_row(const char* label, F&& draw_input) {
    ui::row(label, std::forward<F>(draw_input));
}

[[deprecated("Use asset_ref_field() from ui/asset_ref_widget.hpp")]]
inline bool asset_path_field(const char* label, const char* id, std::string& path) {
    bool changed = false;
    char buf[256];
    std::strncpy(buf, path.c_str(), sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    ImGui::Text("%s", label);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText(id, buf, sizeof(buf));
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        path = buf;
        changed = true;
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const auto* payload = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
            path = std::string(static_cast<const char*>(payload->Data));
            changed = true;
        }
        ImGui::EndDragDropTarget();
    }
    return changed;
}

} // namespace fjell
