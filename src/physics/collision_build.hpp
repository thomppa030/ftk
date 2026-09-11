#pragma once

// Builds collision geometry from render geometry. Engine code, not importer
// code: the mesh editor regenerates collision live while authoring, and the
// importer bakes it at import time, and both must produce identical results.
//
// Depends on meshoptimizer and glm only — no physics backend. The geometry a
// backend receives is produced here.

#include "renderer/resources/fjmesh.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace fjell {

// Render geometry to derive collision from. Positions only; collision never
// needs normals or UVs. Non-owning, like CollisionMesh — the caller holds the
// geometry for the duration of the build.
struct CollisionBuildInput {
    const std::vector<glm::vec3>* vertices{nullptr};
    const std::vector<uint32_t>* indices{nullptr};

    [[nodiscard]] bool valid() const {
        return vertices != nullptr && indices != nullptr &&
               !vertices->empty() && !indices->empty();
    }
};

// Collision geometry as it will be stored in the mesh's collision section.
// `indices` is empty for a convex hull — the points are the hull, and the
// backend builds it — and non-empty for every triangle-based source.
struct CollisionBuildResult {
    std::vector<glm::vec3> vertices;
    std::vector<uint32_t> indices;
    FjMeshCollisionSource source{FjMeshCollisionSource::render_mesh};
};

// Derives the collision geometry `source` calls for. `simplify_ratio` is the
// fraction of triangles to keep and applies to `simplified` only; values at or
// below zero fall back to the format's default.
//
// `render_mesh` and `custom_mesh` return the input unchanged: the former is the
// render geometry by definition, and the latter resolves to a separate .fjmesh
// at load time, so the section keeps this as a fallback.
[[nodiscard]] CollisionBuildResult build_collision(const CollisionBuildInput& in,
                                                   FjMeshCollisionSource source,
                                                   float simplify_ratio);

} // namespace fjell
