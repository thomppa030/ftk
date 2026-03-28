#pragma once

#include <memory>
#include <nlohmann/json_fwd.hpp>

namespace fjell {

struct Node;
class ResourceManager;
class Project;

/// Serialize a single scene node (any type) to JSON.
/// Handles node type discrimination, components, scripts, and transform.
[[nodiscard]] nlohmann::json serialize_node(const Node& node);

/// Deserialize a single scene node from JSON.
/// Returns nullptr on failure (missing model, parse error, etc.).
/// Resource-dependent fields (models, textures, skeletons) are resolved
/// through the provided ResourceManager.
[[nodiscard]] std::unique_ptr<Node> deserialize_node(
    const nlohmann::json& j,
    ResourceManager& resources,
    const Project* project = nullptr);

} // namespace fjell
