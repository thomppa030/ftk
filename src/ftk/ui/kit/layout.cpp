#include "ftk/ui/kit/layout.hpp"

#include <imgui.h>

namespace ftk::ui {

void same_line() {
    ImGui::SameLine();
}

void same_line(float gap) {
    ImGui::SameLine(0.0f, gap);
}

void separator() {
    ImGui::Separator();
}

} // namespace ftk::ui
