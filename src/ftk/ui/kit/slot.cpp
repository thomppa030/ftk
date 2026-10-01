#include "ftk/ui/kit/slot.hpp"

#include "ftk/ui/kit/asset_kind.hpp"
#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/feedback.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/theme.hpp"

#include <algorithm>
#include <filesystem>
#include <string>

namespace ftk::ui {

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

Slot::Slot(const char* payload, const Judge& judge) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float height = ImGui::GetFrameHeight();
    const float width = ImGui::CalcItemWidth();
    // The clear button is always there, disabled while the slot is empty,
    // so the field keeps its width whether it holds something or not.
    const float clear_w = height + style.ItemInnerSpacing.x;
    min_ = ImGui::GetCursorScreenPos();
    max_ = {min_.x + std::max(width - clear_w, height), min_.y + height};

    // The field is one button: clicking anywhere on it opens the picker.
    clicked_ = ImGui::InvisibleButton("##slot", {max_.x - min_.x, height});
    hovered_ = ImGui::IsItemHovered();

    // Something dragged is judged while it is still over the field: what
    // fits lights the field, what doesn't outlines it red and says why, and
    // is not taken.
    if (payload == nullptr || !ImGui::BeginDragDropTarget()) return;
    if (const ImGuiPayload* dragged = ImGui::GetDragDropPayload();
        dragged != nullptr && dragged->IsDataType(payload)) {
        const std::string refusal = judge ? judge(*dragged) : std::string();
        if (refusal.empty()) {
            drop_ = SlotDrop::fits;
            if (const ImGuiPayload* taken = ImGui::AcceptDragDropPayload(
                    payload, ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
                taken != nullptr && taken->IsDelivery()) {
                const auto* bytes = static_cast<const char*>(taken->Data);
                dropped_data_.assign(bytes, bytes + taken->DataSize);
                dropped_ = *taken;
                dropped_.Data = dropped_data_.data();
                has_drop_ = true;
            }
        } else {
            drop_ = SlotDrop::refused;
            refusal_tooltip(refusal);
        }
    }
    ImGui::EndDragDropTarget();
}

bool Slot::finish(SlotFace face, bool empty, std::string_view problem) {
    face.drop = drop_;
    face.hovered = hovered_;
    draw_slot_face(min_, max_, face);

    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::BeginDisabled(empty);
    const bool cleared = icon_button("##clear", icon::clear, empty ? "Nothing to clear" : "Clear",
                                     ButtonKind::GhostDanger);
    ImGui::EndDisabled();

    if (!problem.empty()) {
        status(Severity::Error, std::string(problem).c_str());
    }
    return cleared;
}

SlotFace asset_face(std::string_view file_name, AssetPresence presence, std::string_view empty_extension,
                    std::string& text) {
    SlotFace face;
    switch (presence) {
        case AssetPresence::empty:
            face.glyph = asset_kind(empty_extension).icon;
            face.glyph_colour = theme::text_disabled();
            text = "None";
            face.text_colour = theme::text_secondary();
            break;
        case AssetPresence::missing:
            face.glyph = icon::warning;
            face.glyph_colour = theme::error();
            text = "missing: " + std::string(file_name);
            face.text_colour = theme::error();
            break;
        case AssetPresence::held: {
            const AssetKind& kind = asset_kind(std::filesystem::path(file_name).extension().string());
            face.glyph = kind.icon;
            face.glyph_colour = theme::category(kind.category);
            text = file_name;
            face.text_colour = theme::text();
            break;
        }
    }
    face.text = text;
    return face;
}

} // namespace ftk::ui
