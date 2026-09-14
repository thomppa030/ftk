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
