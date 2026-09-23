#include "ui/imgui_curve_editor.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>

namespace fjell {

// Map curve space [0,1] x [v_min,v_max] to widget pixel coords
static ImVec2 to_screen(float t, float v, const ImVec2& origin, const ImVec2& sz,
                         float v_min, float v_max) {
    float x = origin.x + t * sz.x;
    float y = origin.y + (1.0f - (v - v_min) / (v_max - v_min)) * sz.y;
    return {x, y};
}

// Map widget pixel coords back to curve space
static ImVec2 from_screen(const ImVec2& p, const ImVec2& origin, const ImVec2& sz,
                           float v_min, float v_max) {
    float t = (p.x - origin.x) / sz.x;
    float v = v_min + (1.0f - (p.y - origin.y) / sz.y) * (v_max - v_min);
    return {t, v};
}

bool CurveEditor(const char* label, Curve& curve, ImVec2 size,
                 float v_min, float v_max) {
    bool changed = false;

    ImGui::PushID(label);

    if (size.x <= 0.0f) size.x = ImGui::GetContentRegionAvail().x;

    ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
    ImVec2 canvas_sz = size;

    // Invisible button to capture input
    ImGui::InvisibleButton("##canvas", canvas_sz,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    bool hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Background
    dl->AddRectFilled(canvas_pos, {canvas_pos.x + canvas_sz.x, canvas_pos.y + canvas_sz.y},
                      IM_COL32(30, 30, 30, 255));
    dl->AddRect(canvas_pos, {canvas_pos.x + canvas_sz.x, canvas_pos.y + canvas_sz.y},
                IM_COL32(60, 60, 60, 255));

    // Grid lines
    for (int g = 1; g < 4; ++g) {
        float gy = canvas_pos.y + canvas_sz.y * (static_cast<float>(g) / 4.0f);
        dl->AddLine({canvas_pos.x, gy}, {canvas_pos.x + canvas_sz.x, gy},
                    IM_COL32(50, 50, 50, 255));
        float gx = canvas_pos.x + canvas_sz.x * (static_cast<float>(g) / 4.0f);
        dl->AddLine({gx, canvas_pos.y}, {gx, canvas_pos.y + canvas_sz.y},
                    IM_COL32(50, 50, 50, 255));
    }

    // Draw curve polyline
    constexpr int kSampleCount = 80;
    ImVec2 prev = to_screen(0.0f, curve.evaluate(0.0f), canvas_pos, canvas_sz, v_min, v_max);
    for (int s = 1; s <= kSampleCount; ++s) {
        float t = static_cast<float>(s) / static_cast<float>(kSampleCount);
        ImVec2 pt = to_screen(t, curve.evaluate(t), canvas_pos, canvas_sz, v_min, v_max);
        dl->AddLine(prev, pt, IM_COL32(200, 160, 60, 255), 2.0f);
        prev = pt;
    }

    // What this editor is dragging, kept in ImGui's storage under this
    // editor's own ID so two editors on screen don't share one drag.
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID key_slot = ImGui::GetID("##drag_key");
    const ImGuiID handle_slot = ImGui::GetID("##drag_handle");
    int dragging_key = storage->GetInt(key_slot, -1);
    const int handle = storage->GetInt(handle_slot, 0);   // 0 key, 1 tangent in, 2 tangent out
    bool dragging_tangent_in = handle == 1;
    bool dragging_tangent_out = handle == 2;

    ImVec2 mouse = ImGui::GetIO().MousePos;
    constexpr float kGrabRadius = 6.0f;

    // Draw keyframes + tangent handles
    for (int ki = 0; ki < static_cast<int>(curve.keyframes.size()); ++ki) {
        auto& key = curve.keyframes[ki];
        ImVec2 kp = to_screen(key.time, key.value, canvas_pos, canvas_sz, v_min, v_max);

        // Tangent handle lines + circles
        float tan_len = canvas_sz.x * 0.08f;

        // Tangent in handle (left)
        if (ki > 0) {
            ImVec2 ti_end = {kp.x - tan_len, kp.y + key.tangent_in * tan_len};
            dl->AddLine(kp, ti_end, IM_COL32(100, 180, 100, 180), 1.0f);
            dl->AddCircleFilled(ti_end, 4.0f, IM_COL32(100, 180, 100, 220));

            // Drag tangent in
            if (active && dragging_key == ki && dragging_tangent_in) {
                float dy = -(mouse.y - kp.y) / tan_len;
                key.tangent_in = dy;
                changed = true;
            }
            if (hovered && ImGui::IsMouseClicked(0)) {
                float dx = mouse.x - ti_end.x;
                float dy = mouse.y - ti_end.y;
                if (dx * dx + dy * dy < kGrabRadius * kGrabRadius * 4.0f) {
                    dragging_key = ki;
                    dragging_tangent_in = true;
                    dragging_tangent_out = false;
                }
            }
        }

        // Tangent out handle (right)
        if (ki + 1 < static_cast<int>(curve.keyframes.size())) {
            ImVec2 to_end = {kp.x + tan_len, kp.y - key.tangent_out * tan_len};
            dl->AddLine(kp, to_end, IM_COL32(100, 140, 220, 180), 1.0f);
            dl->AddCircleFilled(to_end, 4.0f, IM_COL32(100, 140, 220, 220));

            // Drag tangent out
            if (active && dragging_key == ki && dragging_tangent_out) {
                float dy = -(mouse.y - kp.y) / tan_len;
                key.tangent_out = dy;
                changed = true;
            }
            if (hovered && ImGui::IsMouseClicked(0)) {
                float dx = mouse.x - to_end.x;
                float dy = mouse.y - to_end.y;
                if (dx * dx + dy * dy < kGrabRadius * kGrabRadius * 4.0f) {
                    dragging_key = ki;
                    dragging_tangent_out = true;
                    dragging_tangent_in = false;
                }
            }
        }

        // Keyframe circle
        dl->AddCircleFilled(kp, kGrabRadius, IM_COL32(220, 180, 60, 255));
        dl->AddCircle(kp, kGrabRadius, IM_COL32(255, 220, 100, 255), 0, 1.5f);

        // Drag keyframe
        if (active && dragging_key == ki && !dragging_tangent_in && !dragging_tangent_out) {
            ImVec2 cv = from_screen(mouse, canvas_pos, canvas_sz, v_min, v_max);
            // First and last keyframe: lock time to 0 and 1
            if (ki == 0) {
                key.time = 0.0f;
            } else if (ki == static_cast<int>(curve.keyframes.size()) - 1) {
                key.time = 1.0f;
            } else {
                float t_min = curve.keyframes[ki - 1].time + 0.001f;
                float t_max = curve.keyframes[ki + 1].time - 0.001f;
                key.time = std::clamp(cv.x, t_min, t_max);
            }
            key.value = std::clamp(cv.y, v_min, v_max);
            changed = true;
        }

        // Start drag on click
        if (hovered && ImGui::IsMouseClicked(0) && dragging_key < 0) {
            float dx = mouse.x - kp.x;
            float dy = mouse.y - kp.y;
            if (dx * dx + dy * dy < kGrabRadius * kGrabRadius) {
                dragging_key = ki;
                dragging_tangent_in = false;
                dragging_tangent_out = false;
            }
        }
    }

    // Release drag
    if (!active || ImGui::IsMouseReleased(0)) {
        dragging_key = -1;
        dragging_tangent_in = false;
        dragging_tangent_out = false;
    }
    storage->SetInt(key_slot, dragging_key);
    storage->SetInt(handle_slot, dragging_tangent_in ? 1 : dragging_tangent_out ? 2 : 0);

    // Right-click: add or delete keyframe
    if (hovered && ImGui::IsMouseClicked(1)) {
        ImVec2 cv = from_screen(mouse, canvas_pos, canvas_sz, v_min, v_max);

        // Check if clicking on existing keyframe → delete
        bool deleted = false;
        for (int ki = 1; ki < static_cast<int>(curve.keyframes.size()) - 1; ++ki) {
            ImVec2 kp = to_screen(curve.keyframes[ki].time, curve.keyframes[ki].value,
                                  canvas_pos, canvas_sz, v_min, v_max);
            float dx = mouse.x - kp.x;
            float dy = mouse.y - kp.y;
            if (dx * dx + dy * dy < kGrabRadius * kGrabRadius * 2.0f) {
                curve.keyframes.erase(curve.keyframes.begin() + ki);
                changed = true;
                deleted = true;
                break;
            }
        }

        // Otherwise add new keyframe
        if (!deleted) {
            float t = std::clamp(cv.x, 0.01f, 0.99f);
            float v = std::clamp(cv.y, v_min, v_max);
            CurveKeyframe nk{t, v, 0.0f, 0.0f};
            auto it = std::lower_bound(curve.keyframes.begin(), curve.keyframes.end(), nk,
                                       [](const CurveKeyframe& a, const CurveKeyframe& b) {
                                           return a.time < b.time;
                                       });
            curve.keyframes.insert(it, nk);
            changed = true;
        }
    }

    ImGui::PopID();
    return changed;
}

} // namespace fjell
