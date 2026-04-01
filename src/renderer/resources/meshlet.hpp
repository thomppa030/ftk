#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace fjell {

struct Vertex;

struct Meshlet {
    uint32_t vertex_offset;     // into meshlet_vertices[]
    uint32_t triangle_offset;   // into meshlet_triangles[] (in triangles, not bytes)
    uint32_t vertex_count;      // max 64
    uint32_t triangle_count;    // max 124
};

struct MeshletBounds {
    glm::vec4 bounding_sphere;    // xyz = center (object space), w = radius
    glm::vec4 cone_apex;          // xyz = apex, w = unused
    glm::vec4 cone_axis_cutoff;   // xyz = axis, w = cutoff (cos half-angle)
};

struct MeshletBuildResult {
    std::vector<Meshlet> meshlets;
    std::vector<MeshletBounds> bounds;
    std::vector<uint32_t> meshlet_vertices;    // global vertex indices
    std::vector<uint8_t> meshlet_triangles;    // 3 bytes per triangle (local indices 0-63)
};

static constexpr uint32_t MAX_MESHLET_VERTICES = 64;
static constexpr uint32_t MAX_MESHLET_TRIANGLES = 124;

MeshletBuildResult build_meshlets(const Vertex* vertices, uint32_t vertex_count,
                                  const uint32_t* indices, uint32_t index_count);

} // namespace fjell
