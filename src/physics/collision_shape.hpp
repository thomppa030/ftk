#pragma once

// Engine-side description of collision geometry, independent of any physics
// backend. Everything that needs a collider's shape — the physics backend,
// viewport picking, debug overlays — reads it through here rather than
// reaching into Model, so the geometry's origin can change without touching
// the consumers.

#include "renderer/resources/fjmesh.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace fjell {

class Model;

// Collision geometry in the mesh's local space. A convex hull is stored as a
// bare point cloud — backends build the hull themselves — so `indices` is
// empty for it and non-empty for every triangle-based source. Whether the
// geometry came from the render mesh, a generated hull, or a separate proxy
// is the source's business, not the consumer's.
struct CollisionMesh {
    const std::vector<glm::vec3>* vertices{nullptr};
    const std::vector<uint32_t>* indices{nullptr};

    // How the geometry was produced. Backends need this to pick a shape
    // type: a hull can back a dynamic body, a triangle mesh cannot.
    FjMeshCollisionSource source{FjMeshCollisionSource::render_mesh};

    // Convex geometry can back a moving body; a triangle mesh is static-only
    // in every backend the engine is likely to use.
    [[nodiscard]] bool supports_dynamic() const {
        return source == FjMeshCollisionSource::convex_hull;
    }

    [[nodiscard]] bool valid() const {
        if (vertices == nullptr || vertices->empty()) return false;
        // A hull needs points only; anything else needs at least one triangle.
        if (supports_dynamic()) return vertices->size() >= 4;
        return indices != nullptr && indices->size() >= 3;
    }

    [[nodiscard]] uint32_t triangle_count() const {
        if (!valid() || indices == nullptr) return 0;
        return static_cast<uint32_t>(indices->size() / 3);
    }
};

// Resolves the collision geometry a model should collide with. Returns an
// invalid CollisionMesh when the model has none.
[[nodiscard]] CollisionMesh collision_mesh_of(const Model& model);

} // namespace fjell
