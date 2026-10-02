#include "ftk/ui/kit/dock.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <unordered_map>
#include <utility>

namespace ftk::ui {

DockNode DockNode::left_right(float ratio, DockNode left, DockNode right) {
    DockNode node{.split = DockAxis::LeftRight, .ratio = ratio};
    node.children.push_back(std::move(left));
    node.children.push_back(std::move(right));
    return node;
}

DockNode DockNode::top_bottom(float ratio, DockNode top, DockNode bottom) {
    DockNode node{.split = DockAxis::TopBottom, .ratio = ratio};
    node.children.push_back(std::move(top));
    node.children.push_back(std::move(bottom));
    return node;
}

DockNode DockNode::tabs(std::initializer_list<std::string> windows) {
    return {.windows = windows};
}

void dockspace(Id id, DockspaceOptions options) {
    ImGuiDockNodeFlags flags = ImGuiDockNodeFlags_None;
    if (options.see_through) flags |= ImGuiDockNodeFlags_PassthruCentralNode;
    if (options.hidden) flags |= ImGuiDockNodeFlags_KeepAliveOnly;
    ImGui::DockSpace(id, {0.0f, 0.0f}, flags);
}

bool dock_exists(Id id) {
    return ImGui::DockBuilderGetNode(id) != nullptr;
}

DockBuild::DockBuild(Id id, glm::vec2 size) : id_{id} {
    ImGui::DockBuilderRemoveNode(id);
    ImGui::DockBuilderAddNode(id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(id, size);
}

DockBuild::~DockBuild() {
    ImGui::DockBuilderFinish(id_);
}

DockSplit split_dock(Id node, DockSide side, float ratio) {
    static constexpr ImGuiDir DIRS[] = {ImGuiDir_Left, ImGuiDir_Right, ImGuiDir_Up, ImGuiDir_Down};
    ImGuiID at_side = 0;
    ImGuiID rest = 0;
    ImGui::DockBuilderSplitNode(node, DIRS[static_cast<std::size_t>(side)], ratio, &at_side, &rest);
    return {at_side, rest};
}

void dock_window(const std::string& name, Id node) {
    ImGui::DockBuilderDockWindow(name.c_str(), node);
}

void place_dock_tree(Id node, const DockNode& tree) {
    // Every split before any window: splitting a node that holds windows
    // would leave them in neither part.
    std::vector<std::pair<const std::string*, Id>> docks;
    const auto place = [&](const auto& self, const DockNode& at, Id id) -> void {
        if (at.split != DockAxis::None && at.children.size() == 2) {
            const DockSide first = at.split == DockAxis::LeftRight ? DockSide::Left : DockSide::Top;
            const DockSplit parts = split_dock(id, first, at.ratio);
            self(self, at.children[0], parts.side);
            self(self, at.children[1], parts.rest);
        }
        for (const std::string& window : at.windows) docks.emplace_back(&window, id);
    };
    place(place, tree, node);
    for (const auto& [window, id] : docks) dock_window(*window, id);
}

void build_dock_tree(Id id, glm::vec2 size, const DockNode& tree) {
    DockBuild build(id, size);
    place_dock_tree(id, tree);
}

std::optional<DockNode> dock_tree(Id id, std::span<const std::string> windows) {
    const ImGuiDockNode* root = ImGui::DockBuilderGetNode(id);
    if (root == nullptr) return std::nullopt;

    // Each window's node: where it is shown, or where the settings file had
    // it when it has not been shown this run.
    std::unordered_map<ImGuiID, std::vector<std::string>> docked;
    for (const std::string& name : windows) {
        ImGuiID node = 0;
        if (const ImGuiWindow* window = ImGui::FindWindowByName(name.c_str())) node = window->DockId;
        if (node == 0) {
            if (const ImGuiWindowSettings* settings = ImGui::FindWindowSettingsByID(ImHashStr(name.c_str()))) {
                node = settings->DockId;
            }
        }
        if (node != 0) docked[node].push_back(name);
    }

    const auto read = [&](const auto& self, const ImGuiDockNode* node) -> DockNode {
        DockNode out;
        if (node->IsSplitNode()) {
            const int axis = node->SplitAxis;
            const float first = node->ChildNodes[0]->SizeRef[axis];
            const float total = first + node->ChildNodes[1]->SizeRef[axis];
            out.split = node->SplitAxis == ImGuiAxis_X ? DockAxis::LeftRight : DockAxis::TopBottom;
            out.ratio = total > 0.0f ? first / total : 0.5f;
            out.children.push_back(self(self, node->ChildNodes[0]));
            out.children.push_back(self(self, node->ChildNodes[1]));
        }
        if (auto it = docked.find(node->ID); it != docked.end()) out.windows = it->second;
        return out;
    };
    return read(read, root);
}

Id window_dock(const std::string& name) {
    const ImGuiWindow* window = ImGui::FindWindowByName(name.c_str());
    return window != nullptr ? window->DockId : 0;
}

} // namespace ftk::ui
