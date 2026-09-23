#pragma once

#include "core/math/color_space.hpp"
#include "ui/kit/button.hpp"
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

/// RAII for a 2-column property table. The label column is fitted to the
/// panel (theme::label_column); the input column stretches to fill. Use inside ImGui::Begin(). Inputs declared
/// via prop_row() get full column width via SetNextItemWidth.
///
///     if (auto t = PropertyTable("##xform")) {
///         prop_row("Position", [&] { ImGui::DragFloat3("##pos", &p.x); });
///         prop_row("Rotation", [&] { ImGui::DragFloat3("##rot", &r.x); });
///     }
class PropertyTable {
public:
    explicit PropertyTable(const char* id) {
        const float label_width = theme::label_column(ImGui::GetContentRegionAvail().x);
        ImGuiTableFlags flags = ImGuiTableFlags_SizingStretchProp
                              | ImGuiTableFlags_NoSavedSettings
                              | ImGuiTableFlags_PadOuterX;
        open_ = ImGui::BeginTable(id, 2, flags);
        if (open_) {
            ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, label_width);
            ImGui::TableSetupColumn("##input", ImGuiTableColumnFlags_WidthStretch);
        }
    }
    ~PropertyTable() {
        if (open_) {
            ImGui::EndTable();
        }
    }

    PropertyTable(const PropertyTable&) = delete;
    PropertyTable& operator=(const PropertyTable&) = delete;
    PropertyTable(PropertyTable&&) = delete;
    PropertyTable& operator=(PropertyTable&&) = delete;

    explicit operator bool() const { return open_; }

private:
    bool open_{false};
};

/// One row inside a PropertyTable: label on the left, input on the right.
/// `draw_input` is called between SetNextItemWidth(-FLT_MIN) and
/// table-cell ending, so the input fills the column.
template <typename F>
inline void prop_row(const char* label, F&& draw_input) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
    std::forward<F>(draw_input)();
}

inline void colored_separator(const ImVec4& color) {
    ImGui::Spacing();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddRectFilled(
        p, {p.x + w, p.y + 2.0f},
        ImGui::ColorConvertFloat4ToU32(color));
    ImGui::Dummy({0.0f, 4.0f});
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

/// A full-width destructive button. Kept for the component block until it
/// gets its header trash icon; new code uses ui::button with a danger kind.
inline bool remove_button(const char* label) {
    ImGui::Spacing();
    const bool pressed = ui::button(label, ui::ButtonKind::Danger, {-1.0f, 0.0f});
    ImGui::Spacing();
    return pressed;
}

} // namespace fjell
