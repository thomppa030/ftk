#include "physics/collision_shape.hpp"

#include "renderer/resources/model.hpp"

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace fjell {

CollisionMesh collision_mesh_of(const Model& model) {
    CollisionMesh mesh;
    mesh.vertices = &model.collision_vertices();
    mesh.indices = &model.collision_indices();
    mesh.source = model.collision_source();
    return mesh;
}

std::vector<glm::vec3> triangle_mesh_edges(const std::vector<glm::vec3>& vertices,
                                           std::span<const uint32_t> indices) {
    std::vector<glm::vec3> lines;
    std::unordered_set<uint64_t> seen;
    seen.reserve(indices.size());
    auto edge = [&](uint32_t a, uint32_t b) {
        if (a > b) std::swap(a, b);
        uint64_t key = (static_cast<uint64_t>(a) << 32) | b;
        if (!seen.insert(key).second) return;
        if (a >= vertices.size() || b >= vertices.size()) return;
        lines.push_back(vertices[a]);
        lines.push_back(vertices[b]);
    };
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        edge(indices[i], indices[i + 1]);
        edge(indices[i + 1], indices[i + 2]);
        edge(indices[i + 2], indices[i]);
    }
    return lines;
}

std::vector<glm::vec3> collision_primitive_edges(const CollisionPrimitive& primitive,
                                                 const CollisionMesh& geometry) {
    if (geometry.vertices == nullptr) return {};
    const auto& verts = *geometry.vertices;

    switch (primitive.kind) {
        case CollisionPrimitiveKind::convex_hull: {
            if (primitive.first_vertex >= verts.size()) return {};
            auto count = std::min<size_t>(primitive.vertex_count,
                                          verts.size() - primitive.first_vertex);
            std::vector<glm::vec3> points(verts.begin() + primitive.first_vertex,
                                          verts.begin() + primitive.first_vertex + count);
            return convex_hull_edges(points);
        }
        case CollisionPrimitiveKind::triangle_mesh: {
            if (geometry.indices == nullptr) return {};
            const auto& idxs = *geometry.indices;
            if (primitive.first_index >= idxs.size()) return {};
            auto count = std::min<size_t>(primitive.index_count,
                                          idxs.size() - primitive.first_index);
            return triangle_mesh_edges(
                verts, std::span<const uint32_t>(idxs).subspan(primitive.first_index, count));
        }
        case CollisionPrimitiveKind::box:
        case CollisionPrimitiveKind::sphere:
        case CollisionPrimitiveKind::capsule:
            return {};
    }
    return {};
}

const std::vector<glm::vec3>& collision_primitive_edges(const Model& model,
                                                        size_t primitive_index) {
    return model.collision_primitive_edges(primitive_index);
}

} // namespace fjell
