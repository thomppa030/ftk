#include "ui/kit/save_bar.hpp"

#include "ui/kit/button.hpp"
#include "ui/kit/icons.hpp"
#include "ui/theme.hpp"

#include <imgui.h>

#include <string>

namespace fjell::ui {

namespace {

constexpr float BAR_HEIGHT = 36.0f;
constexpr float DOT_RADIUS = 3.5f;

} // namespace

float save_bar_height() {
    return BAR_HEIGHT;
}

SaveAction save_bar(bool unsaved, const char* saves) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    dl->AddLine(min, {min.x + width, min.y}, ImGui::GetColorU32(theme::border()));

    const float mid = min.y + BAR_HEIGHT * 0.5f;
    if (unsaved) {
        const float x = min.x + theme::GAP_L;
        dl->AddCircleFilled({x + DOT_RADIUS, mid}, DOT_RADIUS, ImGui::GetColorU32(theme::accent()));
        const char* text = "Unsaved changes";
        dl->AddText({x + DOT_RADIUS * 2.0f + theme::GAP_M, mid - ImGui::GetTextLineHeight() * 0.5f},
                    ImGui::GetColorU32(theme::text_secondary()), text);
    }

    // Revert, then Save, against the right edge.
    const ImGuiStyle& style = ImGui::GetStyle();
    // As ui::action lays its text out: the icon, two spaces, the verb.
    const auto button_width = [&](const char* icon, const char* label) {
        return ImGui::CalcTextSize((std::string(icon) + "  " + label).c_str()).x + style.FramePadding.x * 2.0f;
    };
    const float save_w = button_width(icon::save, "Save");
    const float revert_w = button_width(icon::revert, "Revert");
    const float y = mid - ImGui::GetFrameHeight() * 0.5f;
    ImGui::SetCursorScreenPos({min.x + width - theme::GAP_M - save_w - theme::GAP_S - revert_w, y});

    SaveAction action = SaveAction::None;
    ImGui::BeginDisabled(!unsaved);
    if (ui::action(icon::revert, "Revert", ButtonKind::Ghost)) action = SaveAction::Revert;
    ImGui::SetItemTooltip("%s", unsaved ? "Go back to the saved settings" : "Nothing to revert");
    ImGui::SameLine(0.0f, theme::GAP_S);
    if (ui::action(icon::save, "Save", ButtonKind::Primary)) action = SaveAction::Save;
    const std::string tip = unsaved ? std::string("Writes ") + saves : std::string("Nothing to save");
    ImGui::SetItemTooltip("%s", tip.c_str());
    ImGui::EndDisabled();

    ImGui::SetCursorScreenPos(min);
    ImGui::Dummy({width, BAR_HEIGHT});
    return action;
}

} // namespace fjell::ui
