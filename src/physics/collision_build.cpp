#include "physics/collision_build.hpp"

#include <meshoptimizer.h>

#include <algorithm>
#include <cfloat>
#include <cstddef>

namespace fjell {

namespace {

// Hulls are stored as a bare point cloud, so duplicate positions are pure
// cost — the hull of a set is unchanged by repeating its points.
CollisionBuildResult build_hull(const CollisionBuildInput& in) {
    CollisionBuildResult result;
    result.source = FjMeshCollisionSource::convex_hull;

    result.vertices = *in.vertices;
    std::sort(result.vertices.begin(), result.vertices.end(),
              [](const glm::vec3& a, const glm::vec3& b) {
                  if (a.x != b.x) return a.x < b.x;
                  if (a.y != b.y) return a.y < b.y;
                  return a.z < b.z;
              });
    result.vertices.erase(std::unique(result.vertices.begin(), result.vertices.end()),
                          result.vertices.end());

    // A point cloud has no triangles; the consumer reads the source to know
    // an empty index array is expected rather than a failure.
    return result;
}

CollisionBuildResult build_simplified(const CollisionBuildInput& in, float ratio) {
    const auto& vertices = *in.vertices;
    const auto& indices = *in.indices;

    CollisionBuildResult result;
    result.source = FjMeshCollisionSource::simplified;
    result.vertices = vertices;

    float effective_ratio = ratio > 0.0F ? ratio : 0.1F;
    auto target = static_cast<size_t>(static_cast<float>(indices.size()) * effective_ratio);
    target -= target % 3;
    if (target < 3) target = 3;

    std::vector<uint32_t> simplified(indices.size());
    float error = 0.0F;
    size_t count = meshopt_simplifySloppy(
        simplified.data(),
        indices.data(), indices.size(),
        &vertices[0].x, vertices.size(), sizeof(glm::vec3),
        target, FLT_MAX, &error);
    simplified.resize(count);

    // Sloppy simplification can collapse a mesh entirely at aggressive
    // ratios. Full detail collides correctly, so it is the safe fallback.
    result.indices = count >= 3 ? std::move(simplified) : indices;
    return result;
}

} // namespace

CollisionBuildResult build_collision(const CollisionBuildInput& in,
                                     FjMeshCollisionSource source,
                                     float simplify_ratio) {
    // Nothing to derive from. Returning the (empty) input rather than a
    // default keeps the caller's "collision is whatever came back" contract
    // true for every source.
    if (!in.valid()) {
        CollisionBuildResult empty;
        if (in.vertices != nullptr) empty.vertices = *in.vertices;
        if (in.indices != nullptr) empty.indices = *in.indices;
        empty.source = source;
        return empty;
    }

    CollisionBuildResult unchanged{*in.vertices, *in.indices, source};

    switch (source) {
        case FjMeshCollisionSource::render_mesh:
        case FjMeshCollisionSource::custom_mesh:
            return unchanged;

        case FjMeshCollisionSource::convex_hull:
            return build_hull(in);

        case FjMeshCollisionSource::simplified:
            return build_simplified(in, simplify_ratio);
    }

    return unchanged;
}

} // namespace fjell
