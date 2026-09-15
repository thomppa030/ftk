#include "renderer/resources/sdf_generator.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>
#include <unordered_map>
#include <vector>

namespace fjell {

// ── Point-triangle closest point ────────────────────────────────────────

// Which part of a triangle the closest point lies on. The sign of the
// distance is decided by the normal of that feature, so a query nearest
// to an edge or a corner is classified by every face meeting there, not
// by whichever single face the search happened to visit first.
enum class TriFeature : uint8_t { face, vertex_a, vertex_b, vertex_c, edge_ab, edge_bc, edge_ca };

static glm::vec3 closest_point_on_triangle(const glm::vec3& p,
                                           const glm::vec3& a,
                                           const glm::vec3& b,
                                           const glm::vec3& c,
                                           TriFeature& feature) {
    glm::vec3 ab = b - a, ac = c - a, ap = p - a;
    float d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) { feature = TriFeature::vertex_a; return a; }

    glm::vec3 bp = p - b;
    float d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) { feature = TriFeature::vertex_b; return b; }

    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        float v = d1 / (d1 - d3);
        feature = TriFeature::edge_ab;
        return a + ab * v;
    }

    glm::vec3 cp = p - c;
    float d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) { feature = TriFeature::vertex_c; return c; }

    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        float w = d2 / (d2 - d6);
        feature = TriFeature::edge_ca;
        return a + ac * w;
    }

    float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        feature = TriFeature::edge_bc;
        return b + (c - b) * w;
    }

    float denom = 1.0f / (va + vb + vc);
    float v = vb * denom;
    float w = vc * denom;
    feature = TriFeature::face;
    return a + ab * v + ac * w;
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

    struct ClosestResult {
        float distance;
        uint32_t tri_index;
        glm::vec3 point;      // closest point on that triangle
        TriFeature feature;   // where on the triangle it lies
    };
    ClosestResult find_closest(const glm::vec3& p) const;

private:
    uint32_t build_recursive(uint32_t start, uint32_t count,
                             std::vector<glm::vec3>& centroids);
    void find_closest_recursive(uint32_t node_idx, const glm::vec3& p,
                                float& best_dist_sq, ClosestResult& best) const;
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
    ClosestResult best{0.0f, 0, p, TriFeature::face};
    find_closest_recursive(0, p, best_dist_sq, best);
    best.distance = std::sqrt(best_dist_sq);
    return best;
}

void TriBvh::find_closest_recursive(uint32_t node_idx, const glm::vec3& p,
                                     float& best_dist_sq, ClosestResult& best) const {
    const auto& node = nodes[node_idx];

    // Early out: if this node's AABB is farther than current best, skip
    float aabb_dist_sq = point_aabb_distance_sq(p, node.aabb_min, node.aabb_max);
    if (aabb_dist_sq > best_dist_sq) return;

    // Leaf: test all triangles
    if (node.tri_count > 0) {
        for (uint32_t i = node.tri_start; i < node.tri_start + node.tri_count; ++i) {
            uint32_t t = tri_indices[i];
            TriFeature feature;
            glm::vec3 q = closest_point_on_triangle(p,
                vertices[indices[t * 3]],
                vertices[indices[t * 3 + 1]],
                vertices[indices[t * 3 + 2]], feature);
            glm::vec3 diff = p - q;
            float d_sq = glm::dot(diff, diff);
            if (d_sq < best_dist_sq) {
                best_dist_sq = d_sq;
                best = {0.0f, t, q, feature};
            }
        }
        return;
    }

    // Internal: visit closer child first for tighter pruning
    float dist_left = point_aabb_distance_sq(p, nodes[node.left].aabb_min, nodes[node.left].aabb_max);
    float dist_right = point_aabb_distance_sq(p, nodes[node.right].aabb_min, nodes[node.right].aabb_max);

    if (dist_left <= dist_right) {
        find_closest_recursive(node.left, p, best_dist_sq, best);
        find_closest_recursive(node.right, p, best_dist_sq, best);
    } else {
        find_closest_recursive(node.right, p, best_dist_sq, best);
        find_closest_recursive(node.left, p, best_dist_sq, best);
    }
}

// ── Pseudo-normals ──────────────────────────────────────────────────────

// Angle-weighted pseudo-normals (Bærentzen & Aanæs) give every face, edge and
// vertex a normal whose sign test is exact for the region that feature is
// closest to. A plain face normal is wrong next to concave edges and corners,
// where the nearest face's plane can put an interior point outside.
//
// Meshes arrive with vertices split per face for flat shading and UV seams,
// so positions are welded first: an edge or corner is only shared once its
// endpoints are.
struct PseudoNormals {
    std::vector<uint32_t> weld;             // original vertex -> welded vertex
    std::vector<glm::vec3> face;            // per triangle, unit length
    std::vector<glm::vec3> vertex;          // per welded vertex, angle weighted
    std::unordered_map<uint64_t, glm::vec3> edge; // per welded edge, sum of both faces

    static uint64_t edge_key(uint32_t a, uint32_t b) {
        return (uint64_t(std::min(a, b)) << 32) | uint64_t(std::max(a, b));
    }

    void build(const glm::vec3* vertices, uint32_t vertex_count,
               const uint32_t* indices, uint32_t index_count);

    // Outward normal of the feature a query point is closest to.
    [[nodiscard]] glm::vec3 at(const uint32_t* indices, uint32_t tri, TriFeature feature) const;
};

void PseudoNormals::build(const glm::vec3* vertices, uint32_t vertex_count,
                          const uint32_t* indices, uint32_t index_count) {
    // Weld on a grid one millionth of the mesh extent wide, which merges
    // duplicates from exporters without merging distinct vertices.
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (uint32_t i = 0; i < vertex_count; ++i) {
        lo = glm::min(lo, vertices[i]);
        hi = glm::max(hi, vertices[i]);
    }
    glm::vec3 span = hi - lo;
    float cell = std::max({span.x, span.y, span.z, 1e-6f}) * 1e-6f;

    struct CellHash {
        size_t operator()(const glm::i64vec3& c) const {
            return std::hash<int64_t>()(c.x * 73856093 ^ c.y * 19349663 ^ c.z * 83492791);
        }
    };
    std::unordered_map<glm::i64vec3, uint32_t, CellHash> cells;
    weld.resize(vertex_count);
    uint32_t welded_count = 0;
    for (uint32_t i = 0; i < vertex_count; ++i) {
        glm::i64vec3 c(glm::round((vertices[i] - lo) / cell));
        auto [it, inserted] = cells.try_emplace(c, welded_count);
        if (inserted) ++welded_count;
        weld[i] = it->second;
    }

    uint32_t tri_count = index_count / 3;
    face.assign(tri_count, glm::vec3(0.0f));
    vertex.assign(welded_count, glm::vec3(0.0f));

    for (uint32_t t = 0; t < tri_count; ++t) {
        uint32_t ia = indices[t * 3], ib = indices[t * 3 + 1], ic = indices[t * 3 + 2];
        const glm::vec3& a = vertices[ia];
        const glm::vec3& b = vertices[ib];
        const glm::vec3& c = vertices[ic];
        glm::vec3 n = glm::cross(b - a, c - a);
        float len = glm::length(n);
        if (len <= 0.0f) continue; // degenerate, contributes nothing
        n /= len;
        face[t] = n;

        auto corner_angle = [](const glm::vec3& p, const glm::vec3& q, const glm::vec3& r) {
            glm::vec3 u = glm::normalize(q - p), v = glm::normalize(r - p);
            return std::acos(std::clamp(glm::dot(u, v), -1.0f, 1.0f));
        };
        uint32_t wa = weld[ia], wb = weld[ib], wc = weld[ic];
        vertex[wa] += n * corner_angle(a, b, c);
        vertex[wb] += n * corner_angle(b, c, a);
        vertex[wc] += n * corner_angle(c, a, b);
        edge[edge_key(wa, wb)] += n;
        edge[edge_key(wb, wc)] += n;
        edge[edge_key(wc, wa)] += n;
    }
}

glm::vec3 PseudoNormals::at(const uint32_t* indices, uint32_t tri, TriFeature feature) const {
    uint32_t ia = indices[tri * 3], ib = indices[tri * 3 + 1], ic = indices[tri * 3 + 2];
    switch (feature) {
    case TriFeature::face:     return face[tri];
    case TriFeature::vertex_a: return vertex[weld[ia]];
    case TriFeature::vertex_b: return vertex[weld[ib]];
    case TriFeature::vertex_c: return vertex[weld[ic]];
    case TriFeature::edge_ab:  return edge.at(edge_key(weld[ia], weld[ib]));
    case TriFeature::edge_bc:  return edge.at(edge_key(weld[ib], weld[ic]));
    case TriFeature::edge_ca:  return edge.at(edge_key(weld[ic], weld[ia]));
    }
    return face[tri];
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

    TriBvh bvh;
    bvh.build(vertices, vertex_count, indices, index_count);
    PseudoNormals normals;
    normals.build(vertices, vertex_count, indices, index_count);

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
                float d = closest.distance;

                // Winding is counter-clockwise seen from outside, so the
                // pseudo-normal points out of the surface and a point behind
                // it is inside. An open surface such as a ground plane thereby
                // reads as a half-space: negative on its back side.
                glm::vec3 n = normals.at(indices, closest.tri_index, closest.feature);
                if (glm::dot(n, p - closest.point) < 0.0f) {
                    d = -d;
                }

                float normalized = std::clamp(d / max_dist, -1.0f, 1.0f);
                result.data[z * res * res + y * res + x] =
                    static_cast<int16_t>(normalized * 32767.0f);
            }
        }
    }

    return result;
}

} // namespace fjell
