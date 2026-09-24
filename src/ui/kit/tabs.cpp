#include "ui/kit/tabs.hpp"

#include "ui/kit/icons.hpp"

#include <imgui.h>
#include <imgui_internal.h>

namespace fjell::ui {

namespace {
constexpr float STRIP_PAD = 6.0f;  // above the tabs and before the first
// The tabs name what the panel shows, so they read a step over its text.
constexpr float TAB_TEXT = 16.0f;
constexpr ImVec2 TAB_PADDING{14.0f, 8.0f};
} // namespace

TabStripResult tab_strip(const char* id, const TabStripSpec& spec) {
    TabStripResult result;
    ImGui::PushID(id);
    // Which tab ImGui has in front, to bring the caller's selection forward
    // when it moved without a click (a tab added, one removed).
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID shown_id = ImGui::GetID("##shown");
    const int shown = storage->GetInt(shown_id, -1);

    // The tabs sit in a sunken strip across the panel; the one in front is
    // the panel's own colour, joined to what it shows below, and the rest
    // are its text alone until hovered.
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::PushFont(nullptr, TAB_TEXT);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, TAB_PADDING);
    const float strip_h = STRIP_PAD + ImGui::GetFrameHeight();
    ImGui::GetWindowDrawList()->AddRectFilled(start, {start.x + width, start.y + strip_h},
                                              ImGui::GetColorU32(theme::surface_sunken()));
    ImGui::SetCursorScreenPos({start.x + STRIP_PAD, start.y + STRIP_PAD});
    ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_TabHovered, theme::surface_hover());
    ImGui::PushStyleColor(ImGuiCol_TabSelected, theme::surface_base());
    ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, theme::category(spec.category));
    ImGui::PushStyleColor(ImGuiCol_TabDimmed, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_TabDimmedSelected, theme::surface_base());
    ImGui::PushStyleColor(ImGuiCol_TabDimmedSelectedOverline, theme::category(spec.category));
    const bool bar = ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_FittingPolicyScroll | ImGuiTabBarFlags_NoTooltip |
                                                      ImGuiTabBarFlags_DrawSelectedOverline);
    if (bar) {
        const bool can_remove = spec.names.size() > 1;
        for (int i = 0; i < static_cast<int>(spec.names.size()); ++i) {
            ImGui::PushID(i);
            ImGuiTabItemFlags flags = ImGuiTabItemFlags_None;
            if (i == spec.selected && shown != spec.selected) flags |= ImGuiTabItemFlags_SetSelected;
            bool open = true;
            // The label is only shown: the tab's ID is its place, so a rename keeps it.
            const std::string label = spec.names[static_cast<std::size_t>(i)] + "###tab";
            ImGui::PushStyleColor(ImGuiCol_Text, i == shown ? theme::text() : theme::text_secondary());
            const bool front = ImGui::BeginTabItem(label.c_str(), can_remove ? &open : nullptr, flags);
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) result.rename = i;
            if (can_remove && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", spec.remove_label);
            if (front) {
                // Reported only when the user brought it forward: a selection
                // the caller moved takes a frame to show, and the tab still
                // in front meanwhile is not a pick.
                if (i != shown && shown >= 0 && i != spec.selected) result.selected = i;
                storage->SetInt(shown_id, i);
                ImGui::EndTabItem();
            }
            if (!open) result.removed = i;
            ImGui::PopID();
        }
        const std::string add = std::string(icon::add) + "  " + spec.add_label;
        ImGui::PushStyleColor(ImGuiCol_Text, theme::text_secondary());
        if (ImGui::TabItemButton(add.c_str(), ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip)) {
            result.add = true;
        }
        ImGui::PopStyleColor();
        ImGui::EndTabBar();
    }
    ImGui::PopStyleColor(7);
    ImGui::PopStyleVar();
    ImGui::PopFont();
    // What the tabs show starts under the strip, whatever the bar drew.
    ImGui::SetCursorScreenPos({start.x, start.y + strip_h + theme::GAP_S});
    ImGui::Dummy({width, 0.0f});
    ImGui::PopID();
    return result;
}

ContextTabBar::ContextTabBar(const char* id) {
    // The padding first, so the bar is as tall as a tab with it.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {14.0f, 8.0f});
    const float height = ImGui::GetFrameHeight() + 4.0f;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {4.0f, 2.0f});
    open_ = ImGui::BeginViewportSideBar(id, ImGui::GetMainViewport(), ImGuiDir_Up, height,
                                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    ImVec4 resting = theme::surface_sunken();
    resting.w = 0.0f;
    ImGui::PushStyleColor(ImGuiCol_Tab, resting);
    ImGui::PushStyleColor(ImGuiCol_TabHovered, theme::surface_raised());
    ImGui::PushStyleColor(ImGuiCol_TabSelected, theme::surface_base());
    ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_TabBorderSize, 1.0f);
}

ContextTabBar::~ContextTabBar() {
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();   // the frame padding
    ImGui::End();
}

} // namespace fjell::ui
