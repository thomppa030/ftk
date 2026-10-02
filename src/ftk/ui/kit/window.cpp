#include "ftk/ui/kit/window.hpp"

#include <imgui.h>

namespace ftk::ui::detail {

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

} // namespace ftk::ui::detail
