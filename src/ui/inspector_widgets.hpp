#pragma once

#include <imgui.h>

#include <cstring>
#include <string>

namespace fjell {

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

inline bool remove_button(const char* label) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button, {0.5f, 0.15f, 0.15f, 0.6f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.7f, 0.2f, 0.2f, 0.8f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0.8f, 0.15f, 0.15f, 1.0f});
    bool pressed = ImGui::Button(label, {-1, 0});
    ImGui::PopStyleColor(3);
    ImGui::Spacing();
    return pressed;
}

} // namespace fjell
