#pragma once

#include "scripting/script_manager.hpp" // OwnedFieldDesc
#include "core/string_utils.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace fjell {

// Draw ImGui inspector widgets for reflected fields. Handles value types
// (float, int, bool, vec2/3/4, color3/4, string, enum). Ref types are
// skipped — those require ScriptManager context.
inline void draw_reflected_fields(const std::vector<OwnedFieldDesc>& fields, void* base_ptr) {
    auto* base = static_cast<uint8_t*>(base_ptr);
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const auto& f = fields[i];
        void* ptr = base + f.offset;
        float spd = f.speed > 0.0f ? f.speed : 0.1f;
        std::string label = display_name(f.name);
        const char* lbl = label.c_str();

        ImGui::PushID(static_cast<int>(i));
        switch (f.type) {
            case FJELL_FIELD_FLOAT:
                ImGui::DragFloat(lbl, static_cast<float*>(ptr), spd, f.min, f.max);
                break;
            case FJELL_FIELD_INT:
                ImGui::DragInt(lbl, static_cast<int*>(ptr),
                               spd, static_cast<int>(f.min), static_cast<int>(f.max));
                break;
            case FJELL_FIELD_BOOL:
                ImGui::Checkbox(lbl, static_cast<bool*>(ptr));
                break;
            case FJELL_FIELD_VEC2:
                ImGui::DragFloat2(lbl, static_cast<float*>(ptr), spd, f.min, f.max);
                break;
            case FJELL_FIELD_VEC3:
                ImGui::DragFloat3(lbl, static_cast<float*>(ptr), spd, f.min, f.max);
                break;
            case FJELL_FIELD_VEC4:
                ImGui::DragFloat4(lbl, static_cast<float*>(ptr), spd, f.min, f.max);
                break;
            case FJELL_FIELD_COLOR3:
                ImGui::ColorEdit3(lbl, static_cast<float*>(ptr));
                break;
            case FJELL_FIELD_COLOR4:
                ImGui::ColorEdit4(lbl, static_cast<float*>(ptr));
                break;
            case FJELL_FIELD_STRING:
                ImGui::InputText(lbl, static_cast<char*>(ptr), f.size);
                break;
            case FJELL_FIELD_ENUM:
                if (!f.enum_labels.empty()) {
                    std::string preview = f.enum_labels[
                        std::clamp(*static_cast<int*>(ptr), 0,
                                   static_cast<int>(f.enum_labels.size()) - 1)];
                    if (ImGui::BeginCombo(lbl, preview.c_str())) {
                        for (int e = 0; e < static_cast<int>(f.enum_labels.size()); ++e) {
                            if (ImGui::Selectable(f.enum_labels[e].c_str(),
                                                  *static_cast<int*>(ptr) == e)) {
                                *static_cast<int*>(ptr) = e;
                            }
                        }
                        ImGui::EndCombo();
                    }
                }
                break;
            default:
                break; // Ref types need ScriptManager context
        }
        ImGui::PopID();
    }
}

} // namespace fjell
