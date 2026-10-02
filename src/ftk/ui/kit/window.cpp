#include "ftk/ui/kit/window.hpp"

#include <imgui.h>

namespace ftk::ui {

namespace detail {

int imgui_window_flags(const WindowOptions& options) {
    ImGuiWindowFlags flags = ImGuiWindowFlags_None;
    if (!options.title_bar) flags |= ImGuiWindowFlags_NoTitleBar;
    if (!options.resize) flags |= ImGuiWindowFlags_NoResize;
    if (!options.move) flags |= ImGuiWindowFlags_NoMove;
    if (!options.collapse) flags |= ImGuiWindowFlags_NoCollapse;
    if (!options.scrollbar) flags |= ImGuiWindowFlags_NoScrollbar;
    if (!options.background) flags |= ImGuiWindowFlags_NoBackground;
    if (!options.docking) flags |= ImGuiWindowFlags_NoDocking;
    if (!options.to_front) flags |= ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (!options.nav_focus) flags |= ImGuiWindowFlags_NoNavFocus;
    if (!options.remembered) flags |= ImGuiWindowFlags_NoSavedSettings;
    return flags;
}

} // namespace detail

void next_window_place(glm::vec2 pos, glm::vec2 size) {
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
}

void next_window_size_once(glm::vec2 size) {
    ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);
}

glm::vec2 window_pos() {
    return ImGui::GetWindowPos();
}

glm::vec2 window_size() {
    return ImGui::GetWindowSize();
}

glm::vec2 window_content_min() {
    return glm::vec2(ImGui::GetWindowPos()) + glm::vec2(ImGui::GetWindowContentRegionMin());
}

glm::vec2 window_content_max() {
    return glm::vec2(ImGui::GetWindowPos()) + glm::vec2(ImGui::GetWindowContentRegionMax());
}

bool window_hovered() {
    return ImGui::IsWindowHovered();
}

bool window_or_child_hovered() {
    return ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
}

bool window_under_mouse() {
    return ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
}

bool window_focused() {
    return ImGui::IsWindowFocused();
}

bool window_or_child_focused() {
    return ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
}

float scroll_y() {
    return ImGui::GetScrollY();
}

float scroll_max_y() {
    return ImGui::GetScrollMaxY();
}

void set_scroll_y(float y) {
    ImGui::SetScrollY(y);
}

void scroll_to_cursor(float ratio) {
    ImGui::SetScrollHereY(ratio);
}

} // namespace ftk::ui
