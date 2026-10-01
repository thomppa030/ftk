#pragma once

#include "core/engine_dir.hpp"
#include "ftk/app/imgui_layer.hpp"
#include "ftk/base/log.hpp"

namespace fjell {

/// The ImGui layer's files where Fjell keeps them, with the engine's own: the
/// fonts in engine_assets/fonts and the sRGB fragment stage the build
/// compiles into shaders/. Every Fjell window that draws ImGui (the editor,
/// the hub, the import and file dialogs) takes them from here.
[[nodiscard]] inline ImGuiLayerFiles engine_imgui_files() {
    const auto dir = find_engine_dir();
    if (dir.empty()) {
        FJELL_CORE_ERROR("ImGui: engine directory not found from the executable path; "
                         "fonts and the sRGB fragment stage will be missing");
    }
    return {.fonts = dir / "engine_assets" / "fonts", .srgb_fragment = dir / "shaders" / "imgui.frag.spv"};
}

} // namespace fjell
