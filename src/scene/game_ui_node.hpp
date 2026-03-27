#pragma once

#include "scene/node.hpp"

#include <string>

namespace fjell {

/// Scene node that references a .fjui file for in-game UI rendering.
/// Multiple GameUiNodes per scene allow layered UI (HUD, pause menu, etc.).
struct GameUiNode : Node3D {
    std::string ui_path; // relative path to .fjui (e.g., "assets/ui/hud.fjui")

    [[nodiscard]] std::unique_ptr<Node> clone() const override {
        auto copy = std::make_unique<GameUiNode>();
        clone_3d_into(*copy);
        copy->ui_path = ui_path;
        return copy;
    }
};

} // namespace fjell
