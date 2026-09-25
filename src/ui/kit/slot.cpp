#include "ui/kit/slot.hpp"

#include "ui/kit/icons.hpp"
#include "ui/theme.hpp"

#include <string>

namespace fjell::ui {

namespace {

constexpr float FIELD_PICTURE = 18.0f;

} // namespace

void draw_slot_face(ImVec2 min, ImVec2 max, const SlotFace& face) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float height = max.y - min.y;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rounding = style.FrameRounding;
    dl->AddRectFilled(min, max, ImGui::GetColorU32(face.hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg),
                      rounding);
    if (face.drop == SlotDrop::fits) {
        dl->AddRectFilled(min, max, ImGui::GetColorU32(theme::drop_fits()), rounding);
        dl->AddRect(min, max, ImGui::GetColorU32(theme::accent()), rounding);
    } else if (face.drop == SlotDrop::refused) {
        dl->AddRect(min, max, ImGui::GetColorU32(theme::error()), rounding);
    }

    const float text_y = min.y + style.FramePadding.y;
    float x = min.x + style.FramePadding.x;
    if (face.picture != 0) {
        const float top = min.y + (height - FIELD_PICTURE) * 0.5f;
        dl->AddImage(face.picture, {x, top}, {x + FIELD_PICTURE, top + FIELD_PICTURE});
        x += FIELD_PICTURE + theme::GAP_S + theme::GAP_XS;
    } else if (face.glyph != nullptr) {
        dl->AddText({x, text_y}, ImGui::GetColorU32(face.glyph_colour), face.glyph);
        x += ImGui::CalcTextSize(face.glyph).x + theme::GAP_S + theme::GAP_XS;
    }

    const float chevron = height;
    dl->PushClipRect({x, min.y}, {max.x - chevron, max.y}, true);
    dl->AddText({x, text_y}, ImGui::GetColorU32(face.text_colour), face.text.data(),
                face.text.data() + face.text.size());
    dl->PopClipRect();
    const float chevron_w = ImGui::CalcTextSize(icon::dropdown).x;
    dl->AddText({max.x - chevron + (chevron - chevron_w) * 0.5f, text_y},
                ImGui::GetColorU32(theme::text_secondary()), icon::dropdown);
}

void refusal_tooltip(std::string_view why) {
    if (!ImGui::BeginTooltip()) return;
    ImGui::PushTextWrapPos(theme::TOOLTIP_WRAP);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::error());
    ImGui::TextUnformatted(icon::refused);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextUnformatted(why.data(), why.data() + why.size());
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

} // namespace fjell::ui
