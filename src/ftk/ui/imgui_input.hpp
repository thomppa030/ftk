#pragma once

// The kit's keys, mouse buttons and pointer shapes as Dear ImGui names
// them, for the code beside the kit's ImGui implementation that feeds ImGui
// itself: the harness, which presses the kit's keys into ImGui.

#include "ftk/ui/kit/input.hpp"

#include <imgui.h>

namespace ftk::ui {

[[nodiscard]] ImGuiKey imgui(Key key);
[[nodiscard]] ImGuiMouseButton imgui(MouseButton button);
[[nodiscard]] ImGuiMouseCursor imgui(MouseCursor cursor);

} // namespace ftk::ui
