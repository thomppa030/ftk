#pragma once

#include "ftk/ui/theme.hpp"

#include <imgui.h>

#include <optional>

namespace fjell {

struct Node;

/// What a drag out of the hierarchy carries: the dragged nodes' ids, in
/// scene order, as an array of uint32_t.
inline constexpr const char* NODES_PAYLOAD = "HIERARCHY_NODE";

/// A node type's icon, and the category it is tinted in. A plain node (an
/// empty, a mesh) has none: what it is shows in its component dots.
struct NodeVisuals {
    const char* icon;
    std::optional<theme::Category> category;
    std::optional<ImVec4> tint{};
};

/// How the node is drawn wherever the editor names it: the hierarchy, a
/// node field.
[[nodiscard]] NodeVisuals node_visuals(const Node& node);

} // namespace fjell
