#include "ftk/ui/kit/panel.hpp"

#include "ftk/ui/theme.hpp"

#include <imgui.h>
#include <imgui_internal.h>

namespace ftk::ui {

namespace {

void draw_header(const char* icon, const char* label) {
    // Skip header when the window is docked into a tabbed node — the tab
    // bar already serves as the title and a second header looks redundant.
    // We still draw header for floating / single-pane docked windows.
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    ImGuiDockNode* node = w ? w->DockNode : nullptr;
    if (node && node->TabBar && node->TabBar->Tabs.Size > 1) {
        return;
    }

    constexpr float HEADER_HEIGHT = 24.0f;
    auto* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float w_avail = ImGui::GetContentRegionAvail().x;
    ImVec2 p1 = {p0.x + w_avail, p0.y + HEADER_HEIGHT};

    // Icon + label
    ImGui::SetCursorScreenPos({p0.x, p0.y + 4.0f});
    if (icon != nullptr) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::accent());
        ImGui::TextUnformatted(icon);
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, 8.0f);
    }
    ImGui::PushFont(theme::bold_font());
    // Strip the ImGui "##suffix" disambiguator (e.g. "History##ctx0")
    // the same way the native title bar does — the suffix is for ID
    // uniqueness, not display.
    const char* label_end = ImGui::FindRenderedTextEnd(label);
    ImGui::TextUnformatted(label, label_end);
    ImGui::PopFont();

    // Underline separator beneath the header strip
    ImGui::SetCursorScreenPos({p0.x, p1.y});
    dl->AddLine({p0.x, p1.y}, {p1.x, p1.y},
                ImGui::GetColorU32(ImGuiCol_Separator), 1.0f);
    ImGui::Dummy({0.0f, 4.0f});
}

} // namespace

Panel::Panel(const char* icon, const char* label, bool* p_open, WindowOptions options) {
    options.collapse = false;
    open_ = ImGui::Begin(label, p_open, detail::imgui_window_flags(options));
    if (open_) {
        draw_header(icon, label);
    }
}

Panel::~Panel() {
    ImGui::End();
}

} // namespace ftk::ui
