#include "ui/dockspace_layout.hpp"

#include <imgui_internal.h>

namespace fjell {

void build_dockspace(ImGuiID dock_id, const DockNode& root,
                     const std::vector<std::string>& window_names) {
    struct DockCmd { size_t panel_idx; ImGuiID id; };
    std::vector<DockCmd> cmds;

    auto build = [&](auto& self, const DockNode& node, ImGuiID id) -> void {
        if (node.split_dir != ImGuiDir_None && node.children.size() == 2) {
            ImGuiID child0 = 0;
            ImGuiID child1 = 0;
            ImGui::DockBuilderSplitNode(id, node.split_dir, node.ratio,
                                         &child0, &child1);
            self(self, node.children[0], child0);
            self(self, node.children[1], child1);
        }

        for (size_t idx : node.panels) {
            cmds.push_back({idx, id});
        }
    };

    build(build, root, dock_id);

    for (const auto& cmd : cmds) {
        if (cmd.panel_idx < window_names.size()) {
            ImGui::DockBuilderDockWindow(window_names[cmd.panel_idx].c_str(), cmd.id);
        }
    }
}

} // namespace fjell
