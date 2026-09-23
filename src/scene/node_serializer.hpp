#pragma once

#include <glm/glm.hpp>

#include <memory>
#include <nlohmann/json_fwd.hpp>

namespace fjell {

struct Node;
class ResourceManager;
class Project;

/// Serialize a single scene node (any type) to JSON.
/// Handles node type discrimination, components, scripts, and transform.
/// Asset references are written as ids where the project tracks the asset, so
/// they survive it being moved or renamed; pass the project for that.
[[nodiscard]] nlohmann::json serialize_node(const Node& node,
                                            const Project* project = nullptr);

/// The settings of a node's own type (a light's colour, the fog's reach): the
/// node_type key and the keys those settings live under, without name,
/// transform, components or scripts. Asset paths are written as the node
/// holds them; serialize_node turns them into ids. An empty object for a node
/// with no type of its own.
[[nodiscard]] nlohmann::json serialize_node_type(const Node& node);

/// Sets a typed node's own settings from JSON written by serialize_node_type
/// or serialize_node, replacing what it held. Returns false for a node with no
/// type of its own.
bool apply_node_type(Node& node, const nlohmann::json& j);

/// Deserialize a single scene node from JSON.
/// Returns nullptr on failure (missing model, parse error, etc.).
/// Resource-dependent fields (models, textures, skeletons) are resolved
/// through the provided ResourceManager.
[[nodiscard]] std::unique_ptr<Node> deserialize_node(
    const nlohmann::json& j,
    ResourceManager& resources,
    const Project* project = nullptr);

/// Scene files hold colours as sRGB, the way the inspector shows them; nodes
/// hold linear light, the way the renderer multiplies it. Every colour that
/// crosses between the two goes through this pair.
[[nodiscard]] nlohmann::json color_to_json(glm::vec3 linear);
[[nodiscard]] glm::vec3 color_from_json(const nlohmann::json& srgb);

} // namespace fjell
