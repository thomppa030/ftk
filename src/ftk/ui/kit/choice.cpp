#include "ftk/ui/kit/choice.hpp"

#include "ftk/ui/kit/edit_record.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/theme.hpp"

#include <imgui.h>

#include <algorithm>
#include <string>

namespace ftk::ui::detail {

namespace {

// Room the segmented control keeps around each segment, and the segment's
// corner, inside the sunken track.
constexpr float TRACK_PADDING = 2.0f;
constexpr float SEGMENT_ROUNDING = 3.0f;

float segment_width(float width, std::size_t count) {
    const float gaps = TRACK_PADDING * 2.0f + theme::GAP_XS * static_cast<float>(count - 1);
    return (width - gaps) / static_cast<float>(count);
}

// Where a drop-down's field goes, taken before ImGui draws it: an open
// list becomes the current window.
struct DropdownFace {
    ImVec2 min;
    ImVec2 max;
    ImDrawList* dl;
};

DropdownFace dropdown_face() {
    const ImVec2 min = ImGui::GetCursorScreenPos();
    return {min, {min.x + ImGui::CalcItemWidth(), min.y + ImGui::GetFrameHeight()}, ImGui::GetWindowDrawList()};
}

// ImGui's combo draws its preview and a raised arrow box; the kit's is one
// sunken field with the icon set's chevron inside, drawn over the frame
// ImGui leaves empty.
void draw_dropdown_face(const DropdownFace& face, const char* preview) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float chevron = ImGui::GetFrameHeight();
    const float text_y = face.min.y + style.FramePadding.y;
    face.dl->PushClipRect(face.min, {face.max.x - chevron, face.max.y}, true);
    face.dl->AddText({face.min.x + style.FramePadding.x, text_y}, ImGui::GetColorU32(ImGuiCol_Text), preview);
    face.dl->PopClipRect();
    const float icon_w = ImGui::CalcTextSize(icon::dropdown).x;
    face.dl->AddText({face.max.x - chevron + (chevron - icon_w) * 0.5f, text_y},
                     ImGui::GetColorU32(theme::text_secondary()), icon::dropdown);
}

} // namespace

Edit combo(const char* id, int& index, std::span<const char* const> names) {
    const DropdownFace face = dropdown_face();
    const bool known = index >= 0 && static_cast<std::size_t>(index) < names.size();
    const std::string before = known ? names[static_cast<std::size_t>(index)] : "";

    Edit edit;
    if (ImGui::BeginCombo(id, "", ImGuiComboFlags_NoArrowButton)) {
        for (std::size_t i = 0; i < names.size(); ++i) {
            const bool selected = static_cast<int>(i) == index;
            if (ImGui::Selectable(names[i], selected) && !selected) {
                index = static_cast<int>(i);
                edit = {true, true};
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (edit.committed) {
        detail::track_field(ImGui::GetID(id), before, names[static_cast<std::size_t>(index)], false, edit);
    }
    draw_dropdown_face(face, known ? names[static_cast<std::size_t>(index)] : "");
    return edit;
}

Edit segmented(const char* id, int& index, std::span<const char* const> names) {
    Edit edit;
    if (names.empty()) return edit;

    const float width = ImGui::CalcItemWidth();
    const float height = ImGui::GetFrameHeight();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float segment = segment_width(width, names.size());
    const bool known = index >= 0 && static_cast<std::size_t>(index) < names.size();
    const std::string before = known ? names[static_cast<std::size_t>(index)] : "";

    ImGui::PushID(id);
    dl->AddRectFilled(origin, {origin.x + width, origin.y + height},
                      ImGui::GetColorU32(theme::surface_sunken()), ImGui::GetStyle().FrameRounding);
    ImGui::PushFont(nullptr, theme::SMALL_TEXT);
    for (std::size_t i = 0; i < names.size(); ++i) {
        const float x = origin.x + TRACK_PADDING + static_cast<float>(i) * (segment + theme::GAP_XS);
        const ImVec2 seg_min{x, origin.y + TRACK_PADDING};
        const ImVec2 seg_max{x + segment, origin.y + height - TRACK_PADDING};
        ImGui::SetCursorScreenPos(seg_min);
        ImGui::PushID(static_cast<int>(i));
        const bool on = static_cast<int>(i) == index;
        if (ImGui::InvisibleButton("##segment", {segment, seg_max.y - seg_min.y}) && !on) {
            index = static_cast<int>(i);
            edit = {true, true};
        }
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();

        const bool now_on = static_cast<int>(i) == index;
        if (now_on) {
            dl->AddRectFilled(seg_min, seg_max, ImGui::GetColorU32(theme::toggle_on()), SEGMENT_ROUNDING);
        } else if (hovered) {
            dl->AddRectFilled(seg_min, seg_max, ImGui::GetColorU32(theme::surface_hover()), SEGMENT_ROUNDING);
        }
        const ImVec2 text = ImGui::CalcTextSize(names[i]);
        dl->PushClipRect(seg_min, seg_max, true);
        dl->AddText({x + std::max((segment - text.x) * 0.5f, 0.0f),
                     seg_min.y + (seg_max.y - seg_min.y - text.y) * 0.5f},
                    ImGui::GetColorU32(now_on || hovered ? theme::text() : theme::text_secondary()),
                    names[i]);
        dl->PopClipRect();
    }
    ImGui::PopFont();
    ImGui::PopID();

    // One item the size of the whole control, so the row advances past it
    // and a caller's tooltip or test can aim at it.
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy({width, height});
    if (edit.committed) {
        detail::track_field(ImGui::GetID(id), before, names[static_cast<std::size_t>(index)], false, edit);
    }
    return edit;
}

bool fits_segmented(std::span<const char* const> names) {
    if (names.empty() || names.size() > 3) return false;
    const float segment = segment_width(ImGui::CalcItemWidth(), names.size());
    for (const char* name : names) {
        const float text = ImGui::GetFont()->CalcTextSizeA(theme::SMALL_TEXT, FLT_MAX, 0.0f, name).x;
        if (text + theme::GAP_M * 2.0f > segment) return false;
    }
    return true;
}

} // namespace ftk::ui::detail

namespace ftk::ui {

Dropdown::Dropdown(const char* id, const char* preview) {
    const detail::DropdownFace face = detail::dropdown_face();
    open_ = ImGui::BeginCombo(id, "", ImGuiComboFlags_NoArrowButton);
    detail::draw_dropdown_face(face, preview);
}

Dropdown::~Dropdown() {
    if (open_) ImGui::EndCombo();
}

} // namespace ftk::ui
