#include "ftk/ui/kit/component_block.hpp"

#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/field.hpp"
#include "ftk/ui/kit/icons.hpp"

#include <imgui.h>

#include <string>

namespace ftk::ui {

namespace {

constexpr float BAR_ROUNDING = 1.0f;

} // namespace

namespace {

// The block, with a switch when `enabled` is given.
bool draw_block(const char* name, theme::Category category, bool& remove, bool* enabled) {
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID open_id = ImGui::GetID("##block_open");
    bool open = storage->GetBool(open_id, true);
    const bool on = enabled == nullptr || *enabled;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = ImGui::GetFrameHeight();

    // The category bar, set apart from the block above and close to its own
    // header: GAP_M + GAP_XS above it counting the item spacing, GAP_S under.
    // Grey while the block is switched off.
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float bar_y = p.y + theme::GAP_M + theme::GAP_XS - style.ItemSpacing.y;
    dl->AddRectFilled({p.x, bar_y}, {p.x + width, bar_y + theme::CATEGORY_BAR},
                      ImGui::GetColorU32(on ? theme::category(category) : theme::surface_active()), BAR_ROUNDING);
    ImGui::Dummy({width, bar_y + theme::CATEGORY_BAR + theme::GAP_S - style.ItemSpacing.y - p.y});

    // The header: the whole row but the switch and the trash icon folds the
    // block.
    constexpr float SWITCH_W = 28.0f;
    p = ImGui::GetCursorScreenPos();
    const float trash_x = p.x + width - theme::GAP_S - height;
    const float switch_x = enabled != nullptr ? trash_x - theme::GAP_S - SWITCH_W : trash_x;
    const bool toggled = ImGui::InvisibleButton("##block_header", {switch_x - p.x, height});
    const bool hovered = ImGui::IsItemHovered();
    if (toggled) {
        open = !open;
        storage->SetBool(open_id, open);
    }
    dl->AddRectFilled(p, {p.x + width, p.y + height},
                      ImGui::GetColorU32(hovered ? theme::surface_hover() : theme::surface_raised()),
                      style.FrameRounding);
    const float text_y = p.y + style.FramePadding.y;
    const char* chevron = open ? icon::fold_open : icon::fold_closed;
    const float x = p.x + style.FramePadding.x;
    dl->AddText({x, text_y}, ImGui::GetColorU32(ImGuiCol_Text), chevron);
    dl->PushClipRect(p, {switch_x, p.y + height}, true);
    dl->AddText({x + ImGui::CalcTextSize(chevron).x + theme::GAP_S + theme::GAP_XS, text_y},
                ImGui::GetColorU32(on ? theme::text() : theme::text_disabled()), name);
    dl->PopClipRect();

    if (enabled != nullptr) {
        ImGui::SetCursorScreenPos({switch_x, p.y});
        const std::string tooltip = std::string(*enabled ? "Switch off " : "Switch on ") + name;
        (void)on_off("##block_enabled", *enabled, tooltip.c_str());
    }
    ImGui::SetCursorScreenPos({trash_x, p.y});
    const std::string tooltip = std::string("Remove ") + name;
    if (icon_button("##block_remove", icon::remove, tooltip.c_str(), ButtonKind::GhostDanger)) {
        remove = true;
    }
    return open;
}

} // namespace

bool component_block(const char* name, theme::Category category, bool& remove) {
    return draw_block(name, category, remove, nullptr);
}

bool component_block(const char* name, theme::Category category, bool& remove, bool& enabled) {
    return draw_block(name, category, remove, &enabled);
}

FadedBody::FadedBody(bool faded) : faded_{faded} {
    if (faded_) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.45f);
}

FadedBody::~FadedBody() {
    if (faded_) ImGui::PopStyleVar();
}

} // namespace ftk::ui
