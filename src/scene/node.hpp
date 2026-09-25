#pragma once

#include "scene/component_storage.hpp"
#include "scene/transform.hpp"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace fjell {

class ScriptComponent;

// ── Script entry ────────────────────────────────────────────────────────

struct ScriptEntry {
    std::unique_ptr<ScriptComponent> instance;
    std::string type_name;
    nlohmann::json serialized;
    // Whether this instance's begin_play has run. A script can be started
    // on its own (added to a running game) before the node it is on joins
    // the scene and the rest of its scripts start; it starts only once.
    bool started{false};

    ScriptEntry();
    ~ScriptEntry();
    ScriptEntry(ScriptEntry&&) noexcept;
    ScriptEntry& operator=(ScriptEntry&&) noexcept;
};

// ── Node (base) ─────────────────────────────────────────────────────────
// Pure hierarchy node — id, parent, name, components, scripts.
// No spatial transform. Used for organizational folders.

struct Node3D; // forward declaration for as_spatial()

struct Node {
    uint32_t id{0};
    int32_t parent_id{-1};
    std::string name;
    std::string source;
    std::string preset_source;  // non-empty if instantiated from a .fjp preset

    ComponentStorage components;
    std::vector<ScriptEntry> scripts;

    Node();
    virtual ~Node();
    Node(Node&&) noexcept;
    Node& operator=(Node&&) noexcept;

    [[nodiscard]] bool is_spatial() const { return is_spatial_; }
    [[nodiscard]] inline Node3D* as_spatial();
    [[nodiscard]] inline const Node3D* as_spatial() const;

    [[nodiscard]] virtual std::unique_ptr<Node> clone() const;

protected:
    bool is_spatial_{false};
    void clone_base_into(Node& target) const;
};

// ── Node3D ──────────────────────────────────────────────────────────────
// Spatial node with position, rotation, scale, and world matrix.

struct Node3D : public Node {
    Transform transform;
    glm::mat4 world_matrix_{1.0f};
    glm::mat4 prev_world_matrix_{1.0f}; // previous frame's world matrix (for motion vectors)

    // Cached local matrix — recomputed only when the drawn transform changes
    glm::mat4 local_matrix_{1.0f};
    Transform prev_transform_{}; // what local_matrix_ was built from, for dirty detection
    bool transform_dirty_{true}; // true = local_matrix_ needs recompute
    bool world_dirty_{true};     // true = world_matrix_ needs recompute this frame

    // Blending between fixed steps. The simulation moves a node in whole
    // steps; a node the last step moved is drawn at `interpolated_transform_`,
    // its transform blended between the start and end of that step by how far
    // the frame sits past it, so it moves smoothly at any frame rate. Nothing
    // in the simulation reads these: `transform` stays the exact state.
    Transform step_start_transform_{};
    Transform step_end_transform_{};
    Transform interpolated_transform_{};
    // The step `step_start_transform_` was taken at. A node spawned during
    // the last step has none from that step, and is drawn where it is.
    std::uint64_t step_start_index_{std::numeric_limits<std::uint64_t>::max()};
    // Drawn at `interpolated_transform_` rather than `transform`.
    bool interpolated_{false};

    Node3D();
    ~Node3D() override;
    Node3D(Node3D&&) noexcept;
    Node3D& operator=(Node3D&&) noexcept;

    [[nodiscard]] std::unique_ptr<Node> clone() const override;

protected:
    void clone_3d_into(Node3D& target) const;
};

// Deferred inline definitions — need full Node3D type
inline Node3D* Node::as_spatial() {
    return is_spatial_ ? static_cast<Node3D*>(this) : nullptr;
}
inline const Node3D* Node::as_spatial() const {
    return is_spatial_ ? static_cast<const Node3D*>(this) : nullptr;
}

// Backward compat — most renderer code still references SceneNode
using SceneNode = Node3D;

} // namespace fjell
