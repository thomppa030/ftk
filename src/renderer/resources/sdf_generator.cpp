#include "renderer/resources/sdf_generator.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace fjell {

// ── Point-triangle closest distance ─────────────────────────────────────

static float point_triangle_distance(const glm::vec3& p,
                                      const glm::vec3& a,
                                      const glm::vec3& b,
                                      const glm::vec3& c) {
    glm::vec3 ab = b - a, ac = c - a, ap = p - a;
    float d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return glm::length(p - a);

    glm::vec3 bp = p - b;
    float d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return glm::length(p - b);

    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        float v = d1 / (d1 - d3);
        return glm::length(p - (a + ab * v));
    }

    glm::vec3 cp = p - c;
    float d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return glm::length(p - c);

    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        float w = d2 / (d2 - d6);
        return glm::length(p - (a + ac * w));
    }

    float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return glm::length(p - (b + (c - b) * w));
    }

    float denom = 1.0f / (va + vb + vc);
    float v = vb * denom;
    float w = vc * denom;
    return glm::length(p - (a + ab * v + ac * w));
}

// ── Point-AABB minimum distance ─────────────────────────────────────────

static float point_aabb_distance_sq(const glm::vec3& p,
                                     const glm::vec3& aabb_min,
                                     const glm::vec3& aabb_max) {
    float dx = std::max(0.0f, std::max(aabb_min.x - p.x, p.x - aabb_max.x));
    float dy = std::max(0.0f, std::max(aabb_min.y - p.y, p.y - aabb_max.y));
    float dz = std::max(0.0f, std::max(aabb_min.z - p.z, p.z - aabb_max.z));
    return dx * dx + dy * dy + dz * dz;
}

// ── BVH for closest-triangle queries ────────────────────────────────────

struct BvhNode {
    glm::vec3 aabb_min;
    glm::vec3 aabb_max;
    uint32_t left;       // child index (0 = leaf)
    uint32_t right;      // child index (0 = leaf)
    uint32_t tri_start;  // first triangle in leaf
    uint32_t tri_count;  // number of triangles in leaf (0 = internal)
};

struct TriBvh {
    std::vector<BvhNode> nodes;
    std::vector<uint32_t> tri_indices; // reordered triangle indices

    const glm::vec3* vertices;
    const uint32_t* indices;

    void build(const glm::vec3* verts, uint32_t vert_count,
               const uint32_t* idxs, uint32_t idx_count);

    // Find closest triangle to point. Returns (distance, triangle_index).
    struct ClosestResult { float distance; uint32_t tri_index; };
    ClosestResult find_closest(const glm::vec3& p) const;

private:
    uint32_t build_recursive(uint32_t start, uint32_t count,
                             std::vector<glm::vec3>& centroids);
    void find_closest_recursive(uint32_t node_idx, const glm::vec3& p,
                                float& best_dist_sq, uint32_t& best_tri) const;
};

void TriBvh::build(const glm::vec3* verts, uint32_t /*vert_count*/,
                   const uint32_t* idxs, uint32_t idx_count) {
    vertices = verts;
    indices = idxs;
    uint32_t tri_count = idx_count / 3;

    tri_indices.resize(tri_count);
    std::iota(tri_indices.begin(), tri_indices.end(), 0u);

    // Precompute centroids
    std::vector<glm::vec3> centroids(tri_count);
    for (uint32_t t = 0; t < tri_count; ++t) {
        const auto& a = verts[idxs[t * 3]];
        const auto& b = verts[idxs[t * 3 + 1]];
        const auto& c = verts[idxs[t * 3 + 2]];
        centroids[t] = (a + b + c) / 3.0f;
    }

    nodes.reserve(tri_count * 2);
    build_recursive(0, tri_count, centroids);
}

uint32_t TriBvh::build_recursive(uint32_t start, uint32_t count,
                                  std::vector<glm::vec3>& centroids) {
    uint32_t node_idx = static_cast<uint32_t>(nodes.size());
    nodes.emplace_back();
    auto& node = nodes[node_idx];

    // Compute AABB over all triangles in this range
    node.aabb_min = glm::vec3(1e30f);
    node.aabb_max = glm::vec3(-1e30f);
    for (uint32_t i = start; i < start + count; ++i) {
        uint32_t t = tri_indices[i];
        for (uint32_t v = 0; v < 3; ++v) {
            const auto& pos = vertices[indices[t * 3 + v]];
            node.aabb_min = glm::min(node.aabb_min, pos);
            node.aabb_max = glm::max(node.aabb_max, pos);
        }
    }

    // Leaf: small enough
    if (count <= 4) {
        node.left = 0;
        node.right = 0;
        node.tri_start = start;
        node.tri_count = count;
        return node_idx;
    }

    // Split along the longest axis at the centroid median
    glm::vec3 centroid_min(1e30f), centroid_max(-1e30f);
    for (uint32_t i = start; i < start + count; ++i) {
        centroid_min = glm::min(centroid_min, centroids[tri_indices[i]]);
        centroid_max = glm::max(centroid_max, centroids[tri_indices[i]]);
    }
    glm::vec3 extent = centroid_max - centroid_min;
    int axis = (extent.x >= extent.y && extent.x >= extent.z) ? 0
             : (extent.y >= extent.z) ? 1 : 2;

    // Sort by centroid along the chosen axis
    std::sort(tri_indices.begin() + start,
              tri_indices.begin() + start + count,
              [&](uint32_t a, uint32_t b) {
                  return centroids[a][axis] < centroids[b][axis];
              });

    uint32_t mid = count / 2;
    node.tri_start = 0;
    node.tri_count = 0;

    // Recurse — node reference may be invalidated by push_back, use index
    uint32_t left = build_recursive(start, mid, centroids);
    uint32_t right = build_recursive(start + mid, count - mid, centroids);
    nodes[node_idx].left = left;
    nodes[node_idx].right = right;

    return node_idx;
}

TriBvh::ClosestResult TriBvh::find_closest(const glm::vec3& p) const {
    float best_dist_sq = 1e30f;
    uint32_t best_tri = 0;
    find_closest_recursive(0, p, best_dist_sq, best_tri);
    return {std::sqrt(best_dist_sq), best_tri};
}

void TriBvh::find_closest_recursive(uint32_t node_idx, const glm::vec3& p,
                                     float& best_dist_sq, uint32_t& best_tri) const {
    const auto& node = nodes[node_idx];

    // Early out: if this node's AABB is farther than current best, skip
    float aabb_dist_sq = point_aabb_distance_sq(p, node.aabb_min, node.aabb_max);
    if (aabb_dist_sq > best_dist_sq) return;

    // Leaf: test all triangles
    if (node.tri_count > 0) {
        for (uint32_t i = node.tri_start; i < node.tri_start + node.tri_count; ++i) {
            uint32_t t = tri_indices[i];
            float d = point_triangle_distance(p,
                vertices[indices[t * 3]],
                vertices[indices[t * 3 + 1]],
                vertices[indices[t * 3 + 2]]);
            float d_sq = d * d;
            if (d_sq < best_dist_sq) {
                best_dist_sq = d_sq;
                best_tri = t;
            }
        }
        return;
    }

    // Internal: visit closer child first for tighter pruning
    float dist_left = point_aabb_distance_sq(p, nodes[node.left].aabb_min, nodes[node.left].aabb_max);
    float dist_right = point_aabb_distance_sq(p, nodes[node.right].aabb_min, nodes[node.right].aabb_max);

    if (dist_left <= dist_right) {
        find_closest_recursive(node.left, p, best_dist_sq, best_tri);
        find_closest_recursive(node.right, p, best_dist_sq, best_tri);
    } else {
        find_closest_recursive(node.right, p, best_dist_sq, best_tri);
        find_closest_recursive(node.left, p, best_dist_sq, best_tri);
    }
}

// ── SDF generation ──────────────────────────────────────────────────────

SdfResult generate_sdf(const glm::vec3* vertices, uint32_t vertex_count,
                       const uint32_t* indices, uint32_t index_count,
                       const glm::vec3& aabb_min, const glm::vec3& aabb_max,
                       uint32_t resolution, float margin_fraction) {
    SdfResult result;
    if (vertex_count == 0 || index_count == 0) return result;

    glm::vec3 extent = aabb_max - aabb_min;
    // Ensure no zero-thickness axes (flat geometry like planes)
    // Must be large enough to span multiple voxels in the global SDF volume
    float min_extent = std::max({extent.x, extent.y, extent.z}) * 0.25f;
    if (min_extent < 0.5f) min_extent = 0.5f;
    extent = glm::max(extent, glm::vec3(min_extent));
    glm::vec3 margin = extent * margin_fraction;
    glm::vec3 center = (aabb_min + aabb_max) * 0.5f;
    result.sdf_min = center - extent * 0.5f - margin;
    result.sdf_max = center + extent * 0.5f + margin;
    result.resolution = resolution;
    glm::vec3 sdf_extent = result.sdf_max - result.sdf_min;
    float max_dist = glm::length(sdf_extent);

    // Build BVH over the triangle mesh
    TriBvh bvh;
    bvh.build(vertices, vertex_count, indices, index_count);

    uint32_t res = resolution;
    result.data.resize(res * res * res);

    for (uint32_t z = 0; z < res; ++z) {
        for (uint32_t y = 0; y < res; ++y) {
            for (uint32_t x = 0; x < res; ++x) {
                glm::vec3 p = result.sdf_min + sdf_extent * glm::vec3(
                    (float(x) + 0.5f) / float(res),
                    (float(y) + 0.5f) / float(res),
                    (float(z) + 0.5f) / float(res));

                auto closest = bvh.find_closest(p);
                float min_d = closest.distance;

                // Sign: check if point is on the back side of the closest triangle
                uint32_t t = closest.tri_index;
                const glm::vec3& a = vertices[indices[t * 3]];
                const glm::vec3& b = vertices[indices[t * 3 + 1]];
                const glm::vec3& c = vertices[indices[t * 3 + 2]];
                glm::vec3 face_normal = glm::cross(b - a, c - a);
                // CW winding (Vulkan convention) — normal points inward, flip sign
                if (glm::dot(face_normal, p - a) > 0.0f) {
                    min_d = -min_d;
                }

                float normalized = std::clamp(min_d / max_dist, -1.0f, 1.0f);
                result.data[z * res * res + y * res + x] =
                    static_cast<int16_t>(normalized * 32767.0f);
            }
        }
    }

    return result;
}

} // namespace fjell
