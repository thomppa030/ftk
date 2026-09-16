#pragma once

#include "renderer/frustum.hpp"
#include "renderer/resources/resource_handle.hpp"
#include "scene/node_component.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>
#include <vector>

namespace fjell {

class Model;
class Material;

/// One copy of the model, in the owning node's local space. Kept as position,
/// rotation and scale rather than a matrix so the editor can show and change
/// each exactly; the matrix the renderer wants is derived.
struct InstancePlacement {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};

    /// The scene node this placement was put on, 0 for none, and how far
    /// along that surface's normal it sits. A placement anchored to a
    /// terrain follows the ground when the terrain is sculpted under it.
    uint32_t anchor_node{0};
    float surface_offset{0.0f};

    [[nodiscard]] glm::mat4 matrix() const;
};

/// Many copies of one model with one material: rocks, trees, props. The
/// placements are bulk data and live in a `.fjinst` beside the scene, which
/// the scene references by path; the scene file itself holds only that
/// path and the material.
struct InstancedMeshComponent : NodeComponent {
    ModelHandle model_handle;
    MaterialHandle material_handle;
    Model* model{nullptr};
    Material* material{nullptr};
    std::string material_name;

    /// The placements, in node-local space. This is what edits change and
    /// what the file holds.
    std::vector<InstancePlacement> placements;

    /// Project-relative path of the `.fjinst` holding `placements`. Empty
    /// until the first save, which creates one beside the scene.
    std::string instances_path;

    /// The placements as matrices, for the renderer. Rebuilt from
    /// `placements` when `dirty`; never edited directly.
    std::vector<glm::mat4> instances;

    // Union of every instance's model-local-AABB transformed by its local-space
    // instance matrix, expressed in the component's local (node-parent) space.
    // Cached across frames; recomputed when `dirty` is set by editor mutations
    // or when the model changes.
    AABB local_bounds{};

    /// `placements` or the model changed: rebuild `instances` and the
    /// bounds before the next draw.
    bool dirty{true};
    /// `placements` changed since the file was last written; the scene save
    /// writes it out.
    bool file_dirty{false};

    /// Note a change to `placements`.
    void mark_placements_changed() {
        dirty = true;
        file_dirty = true;
    }

    /// Bring `instances` up to date with `placements`.
    void rebuild_instances();
};

} // namespace fjell
