#include "renderer/meshlet.hpp"
#include "renderer/vertex.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace fjell {

// Build triangle adjacency: for each triangle, find triangles sharing an edge
static std::vector<std::vector<uint32_t>> build_adjacency(
    const uint32_t* indices, uint32_t triangle_count) {
    // Edge → triangle list
    struct EdgeHash {
        size_t operator()(std::pair<uint32_t, uint32_t> e) const {
            return std::hash<uint64_t>{}(
                (static_cast<uint64_t>(e.first) << 32) | e.second);
        }
    };

    std::unordered_map<std::pair<uint32_t, uint32_t>, std::vector<uint32_t>, EdgeHash> edge_tris;

    for (uint32_t t = 0; t < triangle_count; ++t) {
        uint32_t i0 = indices[t * 3 + 0];
        uint32_t i1 = indices[t * 3 + 1];
        uint32_t i2 = indices[t * 3 + 2];

        auto edge = [](uint32_t a, uint32_t b) -> std::pair<uint32_t, uint32_t> {
            return {std::min(a, b), std::max(a, b)};
        };

        edge_tris[edge(i0, i1)].push_back(t);
        edge_tris[edge(i1, i2)].push_back(t);
        edge_tris[edge(i2, i0)].push_back(t);
    }

    std::vector<std::vector<uint32_t>> adj(triangle_count);
    for (const auto& [e, tris] : edge_tris) {
        for (size_t i = 0; i < tris.size(); ++i) {
            for (size_t j = i + 1; j < tris.size(); ++j) {
                adj[tris[i]].push_back(tris[j]);
                adj[tris[j]].push_back(tris[i]);
            }
        }
    }
    return adj;
}

// Ritter's bounding sphere
static glm::vec4 compute_bounding_sphere(const Vertex* vertices,
                                          const uint32_t* vert_indices,
                                          uint32_t count) {
    if (count == 0) return glm::vec4(0.0f);

    // Start with centroid
    glm::vec3 center{0.0f};
    for (uint32_t i = 0; i < count; ++i) {
        center += vertices[vert_indices[i]].position;
    }
    center /= static_cast<float>(count);

    // Find farthest point from centroid
    float radius = 0.0f;
    for (uint32_t i = 0; i < count; ++i) {
        float d = glm::distance(center, vertices[vert_indices[i]].position);
        radius = std::max(radius, d);
    }

    // Ritter expansion pass
    for (uint32_t i = 0; i < count; ++i) {
        glm::vec3 p = vertices[vert_indices[i]].position;
        float d = glm::distance(center, p);
        if (d > radius) {
            float new_radius = (radius + d) * 0.5f;
            float k = (new_radius - radius) / d;
            center += k * (p - center);
            radius = new_radius;
        }
    }

    return glm::vec4(center, radius);
}

// Compute normal cone for backface culling
static void compute_cone(const Vertex* vertices, const uint32_t* indices,
                         uint32_t triangle_count, const glm::vec4& sphere,
                         glm::vec4& cone_apex, glm::vec4& cone_axis_cutoff) {
    if (triangle_count == 0) {
        cone_apex = glm::vec4(0.0f);
        cone_axis_cutoff = glm::vec4(0.0f, 0.0f, 1.0f, -1.0f);
        return;
    }

    // Average face normal
    glm::vec3 avg_normal{0.0f};
    for (uint32_t t = 0; t < triangle_count; ++t) {
        glm::vec3 v0 = vertices[indices[t * 3 + 0]].position;
        glm::vec3 v1 = vertices[indices[t * 3 + 1]].position;
        glm::vec3 v2 = vertices[indices[t * 3 + 2]].position;
        avg_normal += glm::cross(v1 - v0, v2 - v0);
    }

    float len = glm::length(avg_normal);
    if (len < 1e-6f) {
        cone_apex = glm::vec4(glm::vec3(sphere), 0.0f);
        cone_axis_cutoff = glm::vec4(0.0f, 0.0f, 1.0f, -1.0f); // degenerate, never culled
        return;
    }
    glm::vec3 axis = avg_normal / len;

    // Cone apex: center + axis * radius (approximate)
    glm::vec3 center = glm::vec3(sphere);
    float radius = sphere.w;
    cone_apex = glm::vec4(center + axis * radius, 0.0f);

    // Find worst-case dot product (minimum cos angle between axis and any face normal)
    float min_dot = 1.0f;
    for (uint32_t t = 0; t < triangle_count; ++t) {
        glm::vec3 v0 = vertices[indices[t * 3 + 0]].position;
        glm::vec3 v1 = vertices[indices[t * 3 + 1]].position;
        glm::vec3 v2 = vertices[indices[t * 3 + 2]].position;
        glm::vec3 face_normal = glm::normalize(glm::cross(v1 - v0, v2 - v0));
        min_dot = std::min(min_dot, glm::dot(axis, face_normal));
    }

    // If the cone is too wide (> ~168 degrees), disable culling for this meshlet
    if (min_dot <= 0.1f) {
        cone_axis_cutoff = glm::vec4(axis, 1.0f); // cutoff=1 means never culled
        return;
    }

    // meshoptimizer convention: the cutoff is sin(a) = sqrt(1 - cos²(a))
    // where a is the half-angle of the normal cone. The test in the shader
    // checks dot(normalize(center - camera_pos), axis) >= cutoff.
    float cutoff = std::sqrt(1.0f - min_dot * min_dot);
    cone_axis_cutoff = glm::vec4(axis, cutoff);
}

MeshletBuildResult build_meshlets(const Vertex* vertices, uint32_t vertex_count,
                                  const uint32_t* indices, uint32_t index_count) {
    MeshletBuildResult result;

    uint32_t triangle_count = index_count / 3;
    if (triangle_count == 0) return result;

    auto adjacency = build_adjacency(indices, triangle_count);
    std::vector<bool> used(triangle_count, false);
    uint32_t remaining = triangle_count;

    while (remaining > 0) {
        // Find seed: first unused triangle
        uint32_t seed = 0;
        for (uint32_t t = 0; t < triangle_count; ++t) {
            if (!used[t]) { seed = t; break; }
        }

        // Greedy grow meshlet
        std::unordered_set<uint32_t> meshlet_verts;
        std::vector<uint32_t> meshlet_tris;

        // BFS frontier
        std::vector<uint32_t> frontier = {seed};
        used[seed] = true;
        remaining--;

        // Add seed triangle
        for (int k = 0; k < 3; ++k) {
            meshlet_verts.insert(indices[seed * 3 + k]);
        }
        meshlet_tris.push_back(seed);

        while (!frontier.empty() &&
               meshlet_tris.size() < MAX_MESHLET_TRIANGLES) {
            std::vector<uint32_t> next_frontier;

            for (uint32_t tri : frontier) {
                for (uint32_t neighbor : adjacency[tri]) {
                    if (used[neighbor]) continue;
                    if (meshlet_tris.size() >= MAX_MESHLET_TRIANGLES) break;

                    // Count new vertices this triangle would add
                    uint32_t new_verts = 0;
                    for (int k = 0; k < 3; ++k) {
                        if (meshlet_verts.find(indices[neighbor * 3 + k]) ==
                            meshlet_verts.end()) {
                            new_verts++;
                        }
                    }

                    if (meshlet_verts.size() + new_verts > MAX_MESHLET_VERTICES) {
                        continue;
                    }

                    used[neighbor] = true;
                    remaining--;
                    for (int k = 0; k < 3; ++k) {
                        meshlet_verts.insert(indices[neighbor * 3 + k]);
                    }
                    meshlet_tris.push_back(neighbor);
                    next_frontier.push_back(neighbor);
                }
            }

            frontier = std::move(next_frontier);
        }

        // Build meshlet data
        // Map global vertex indices → local (0-63) indices
        std::vector<uint32_t> vert_list(meshlet_verts.begin(), meshlet_verts.end());
        std::sort(vert_list.begin(), vert_list.end());

        std::unordered_map<uint32_t, uint32_t> global_to_local;
        for (uint32_t i = 0; i < static_cast<uint32_t>(vert_list.size()); ++i) {
            global_to_local[vert_list[i]] = i;
        }

        Meshlet m{};
        m.vertex_offset = static_cast<uint32_t>(result.meshlet_vertices.size());
        m.triangle_offset = static_cast<uint32_t>(result.meshlet_triangles.size() / 3);
        m.vertex_count = static_cast<uint32_t>(vert_list.size());
        m.triangle_count = static_cast<uint32_t>(meshlet_tris.size());

        // Append vertex indices
        for (uint32_t v : vert_list) {
            result.meshlet_vertices.push_back(v);
        }

        // Append triangle local indices (3 uint8 per triangle)
        for (uint32_t tri : meshlet_tris) {
            for (int k = 0; k < 3; ++k) {
                uint32_t global_idx = indices[tri * 3 + k];
                result.meshlet_triangles.push_back(
                    static_cast<uint8_t>(global_to_local[global_idx]));
            }
        }

        // Compute bounds
        MeshletBounds bounds{};
        bounds.bounding_sphere = compute_bounding_sphere(
            vertices, vert_list.data(), m.vertex_count);

        // Build index list for cone computation (global indices for this meshlet's tris)
        std::vector<uint32_t> tri_indices;
        tri_indices.reserve(meshlet_tris.size() * 3);
        for (uint32_t tri : meshlet_tris) {
            tri_indices.push_back(indices[tri * 3 + 0]);
            tri_indices.push_back(indices[tri * 3 + 1]);
            tri_indices.push_back(indices[tri * 3 + 2]);
        }
        compute_cone(vertices, tri_indices.data(), m.triangle_count,
                     bounds.bounding_sphere, bounds.cone_apex, bounds.cone_axis_cutoff);

        result.meshlets.push_back(m);
        result.bounds.push_back(bounds);
    }

    (void)vertex_count; // used implicitly via vertices[]
    return result;
}

} // namespace fjell
