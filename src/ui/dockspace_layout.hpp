#pragma once

#include <imgui.h>

#include <initializer_list>
#include <string>
#include <vector>

namespace fjell {

// Recursive dockspace layout tree — mirrors the .ctx.json format.
// Branch nodes split in x or y; leaf nodes hold panel indices.
// Panel type is templated so each context uses its own enum.
struct DockNode {
    ImGuiDir split_dir = ImGuiDir_None;
    float ratio = 0.5f;
    std::vector<DockNode> children;
    std::vector<size_t> panels;

    static DockNode horizontal_split(float r, DockNode left, DockNode right) {
        DockNode n;
        n.split_dir = ImGuiDir_Left;
        n.ratio = r;
        n.children.push_back(std::move(left));
        n.children.push_back(std::move(right));
        return n;
    }

    static DockNode vertical_split(float r, DockNode top, DockNode bottom) {
        DockNode n;
        n.split_dir = ImGuiDir_Up;
        n.ratio = r;
        n.children.push_back(std::move(top));
        n.children.push_back(std::move(bottom));
        return n;
    }

    template <typename PanelEnum>
    static DockNode leaf(std::initializer_list<PanelEnum> p) {
        DockNode n;
        for (auto v : p)
            n.panels.push_back(static_cast<size_t>(v));
        return n;
    }
};

void build_dockspace(ImGuiID dock_id, const DockNode& root,
                     const std::vector<std::string>& window_names);

} // namespace fjell
