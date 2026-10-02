#include "ftk/ui/kit/tooltip.hpp"

#include "ftk/ui/theme.hpp"

#include <imgui.h>
#include <imgui_internal.h>

namespace ftk::ui {

void item_tooltip(std::string_view text, std::string_view shortcut) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) tooltip(text, shortcut);
}

void tooltip(std::string_view text, std::string_view shortcut) {
    // In place of any tooltip asked for earlier this frame, as ImGui's own
    // SetTooltip() does.
    if (!ImGui::BeginTooltipEx(ImGuiTooltipFlags_OverridePrevious, ImGuiWindowFlags_None)) return;
    ImGui::PushTextWrapPos(theme::TOOLTIP_WRAP);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopTextWrapPos();
    if (!shortcut.empty()) {
        ImGui::SameLine(0.0f, theme::GAP_L);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::text_secondary());
        ImGui::TextUnformatted(shortcut.data(), shortcut.data() + shortcut.size());
        ImGui::PopStyleColor();
    }
    ImGui::EndTooltip();
}

ItemTooltip::ItemTooltip() : open_{ImGui::BeginItemTooltip()} {
    if (open_) ImGui::PushTextWrapPos(theme::TOOLTIP_WRAP);
}

ItemTooltip::~ItemTooltip() {
    if (!open_) return;
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

} // namespace ftk::ui
