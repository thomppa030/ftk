#include "ui/kit/inset_group.hpp"

#include "ui/kit/icons.hpp"
#include "ui/theme.hpp"

#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <string>

namespace fjell::ui {

namespace {

// The box's inner padding: a little more under the last row.
constexpr float PAD = theme::GAP_S + theme::GAP_XS;
constexpr float PAD_BOTTOM = theme::GAP_M;
constexpr float HEADING_SIZE = 11.5f;

} // namespace

InsetGroup::InsetGroup(const char* id, const char* heading, const char* icon, const ImVec4& icon_colour) {
    ImGui::PushID(id);
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID open_id = ImGui::GetID("##open");
    height_id_ = ImGui::GetID("##height");
    open_ = storage->GetBool(open_id, true);

    min_ = ImGui::GetCursorScreenPos();
    width_ = ImGui::GetContentRegionAvail().x;
    // The backdrop goes behind the contents, whose height is only known once
    // they are drawn: it is drawn at the height the box had last frame.
    const float heading_h = ImGui::GetFrameHeight();
    const float height = open_ ? storage->GetFloat(height_id_, heading_h + PAD * 2.0f) : heading_h + PAD;
    ImGui::GetWindowDrawList()->AddRectFilled(min_, {min_.x + width_, min_.y + height},
                                              ImGui::GetColorU32(theme::surface_sunken()),
                                              ImGui::GetStyle().FrameRounding);

    // The heading: the whole row folds the box.
    ImGui::SetCursorScreenPos({min_.x, min_.y + PAD * 0.5f});
    if (ImGui::InvisibleButton("##fold", {std::max(width_, 1.0f), heading_h})) {
        open_ = !open_;
        storage->SetBool(open_id, open_);
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushFont(theme::bold_font(), HEADING_SIZE);
    const float y = min_.y + PAD * 0.5f + (heading_h - ImGui::GetFontSize()) * 0.5f;
    float x = min_.x + PAD;
    const ImU32 secondary = ImGui::GetColorU32(theme::text_secondary());
    const char* chevron = open_ ? icon::fold_open : icon::fold_closed;
    dl->AddText({x, y}, secondary, chevron);
    x += ImGui::CalcTextSize(chevron).x + theme::GAP_S;
    if (icon != nullptr) {
        dl->AddText({x, y}, ImGui::GetColorU32(icon_colour), icon);
        x += ImGui::CalcTextSize(icon).x + theme::GAP_S + theme::GAP_XS;
    }
    std::string text(heading);
    for (char& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    dl->PushClipRect(min_, {min_.x + width_ - PAD, min_.y + heading_h + PAD}, true);
    dl->AddText({x, y}, secondary, text.c_str());
    dl->PopClipRect();
    ImGui::PopFont();

    if (!open_) {
        ImGui::SetCursorScreenPos({min_.x, min_.y + height});
        ImGui::Dummy({width_, 0.0f});
        ImGui::PopID();
        return;
    }

    // The contents, inside the padding, on the darker field colour a sunken
    // box needs; anything sized to the space left ends at the padding.
    ImGui::SetCursorScreenPos({min_.x + PAD, min_.y + PAD * 0.5f + heading_h + theme::GAP_XS});
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_FrameBg, theme::surface_inset());
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, theme::surface_base());
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    work_right_ = window->WorkRect.Max.x;
    content_right_ = window->ContentRegionRect.Max.x;
    const float right = min_.x + width_ - PAD;
    window->WorkRect.Max.x = right;
    window->ContentRegionRect.Max.x = right;
}

InsetGroup::~InsetGroup() {
    if (!open_) return;
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    window->WorkRect.Max.x = work_right_;
    window->ContentRegionRect.Max.x = content_right_;
    ImGui::PopStyleColor(2);
    ImGui::EndGroup();
    const float height = ImGui::GetItemRectMax().y - min_.y + PAD_BOTTOM;
    ImGui::GetStateStorage()->SetFloat(height_id_, height);
    ImGui::SetCursorScreenPos({min_.x, min_.y + height});
    ImGui::Dummy({width_, 0.0f});
    ImGui::PopID();
}

} // namespace fjell::ui
