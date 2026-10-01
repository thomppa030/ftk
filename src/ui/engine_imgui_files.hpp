#pragma once

#include "ftk/app/imgui_layer.hpp"
#include "ftk/platform/platform.hpp"

namespace fjell {

/// The ImGui layer's files, which ftk_stage_editor_data() stages beside the
/// binary in ftk/: the theme's fonts and the sRGB fragment stage. Every Fjell
/// window that draws ImGui (the editor, the hub, the import and file dialogs)
/// takes them from here.
[[nodiscard]] inline ftk::ImGuiLayerFiles engine_imgui_files() {
    const auto dir = ftk::platform::executable_path().parent_path() / "ftk";
    return {.fonts = dir / "fonts", .srgb_fragment = dir / "imgui.frag.spv"};
}

} // namespace fjell
