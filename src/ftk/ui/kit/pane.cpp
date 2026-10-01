#include "ftk/ui/kit/pane.hpp"

#include "ftk/ui/theme.hpp"

namespace fjell::ui {

Pane::Pane(const char* id, ImVec2 size, PaneSurface surface) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg,
                          surface == PaneSurface::Sunken ? theme::surface_sunken() : theme::surface_base());
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {theme::GAP_L, theme::GAP_L});
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    visible_ = ImGui::BeginChild(id, size, ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

Pane::~Pane() {
    ImGui::EndChild();
}

} // namespace fjell::ui
