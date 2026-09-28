#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace fjell {

class Scene;
struct Node;

struct NodeSlotConfig {
    const char* id;               ///< ImGui ID, e.g. "##target"
    const Scene* scene{nullptr};  ///< the nodes it offers and names
    /// Why a node cannot go in the slot, or empty when it can. The picker
    /// offers only nodes this accepts, and a dragged node it turns down is
    /// refused with its answer. Empty takes any node.
    std::function<std::string(const Node&)> refusal{};
    /// What the field reads while it holds nothing, and its icon: "None", or
    /// the default an empty slot stands for ("Local player").
    const char* empty_glyph{nullptr};
    std::string_view empty_text{"None"};
    /// Empty is a problem: the scene cannot play until the slot is set.
    bool required{false};
};

/// A field holding a scene node, for the value column of a property row: the
/// node's icon and name as the hierarchy shows them (its parent path in the
/// tooltip), and while it holds nothing, `empty_text`. Clicking it opens a
/// searchable picker of the nodes the slot takes, each with its parent path;
/// a node dragged from the hierarchy is taken if it fits and refused, with
/// the reason, if it doesn't. A clear button follows it, disabled while it is
/// empty. A held node that is gone reads "missing: the node is gone", one
/// that no longer fits says why under the field, and a `required` slot left
/// empty says it is required. Returns true when `node`
/// changed, 0 meaning empty; every change is a finished edit.
bool node_slot(const NodeSlotConfig& config, uint32_t& node);

/// Where the node sits: its ancestors' names from the root down, " / "
/// between them; empty for a node at the root.
[[nodiscard]] std::string node_parent_path(const Scene& scene, const Node& node);

} // namespace fjell
