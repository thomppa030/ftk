#include "ftk/ui/kit/region.hpp"

#include <imgui.h>

namespace ftk::ui {

Region::Region(const char* id, glm::vec2 size, RegionOptions options) {
    ImGuiChildFlags child = ImGuiChildFlags_None;
    if (options.border) child |= ImGuiChildFlags_Borders;
    if (options.resize_x) child |= ImGuiChildFlags_ResizeX;
    ImGuiWindowFlags window = ImGuiWindowFlags_None;
    if (options.scroll_x) window |= ImGuiWindowFlags_HorizontalScrollbar;
    if (!options.wheel_scrolls) window |= ImGuiWindowFlags_NoScrollWithMouse;
    visible_ = ImGui::BeginChild(id, size, child, window);
}

Region::~Region() {
    // Ended whether or not it began visible, as ImGui asks.
    ImGui::EndChild();
}

} // namespace ftk::ui
