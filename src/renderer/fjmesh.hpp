#pragma once

// .fjmesh binary format — Fjell engine internal mesh format.
// Shared between the engine runtime and the fjimport tool.
// No engine dependencies beyond <cstdint> and <glm/glm.hpp>.

#include <glm/glm.hpp>

#include <cstdint>

namespace fjell {

static constexpr uint32_t FJMESH_MAGIC   = 0x484D4A46; // "FJMH" little-endian
static constexpr uint32_t FJMESH_VERSION = 2;

// Maximum LOD levels stored in the file (LOD 0 = full detail)
static constexpr uint32_t MAX_LOD_LEVELS = 4;

struct FjMeshSubmesh {
    uint32_t first_meshlet;    // index into the file's meshlet array
    uint32_t meshlet_count;
    uint32_t vertex_offset;    // into the file's vertex array
    uint32_t first_index;      // into the file's index array
    uint32_t index_count;
    uint32_t material_slot;    // material index from the source model
};

// Per-LOD level metadata: points into the flat submesh array
struct FjMeshLodLevel {
    uint32_t first_submesh;    // index into the submesh array
    uint32_t submesh_count;    // number of submeshes at this LOD
    float max_screen_size;     // normalized screen-space threshold (0..1); 1.0 for LOD 0
    uint32_t pad;              // alignment padding
};

struct FjMeshHeader {
    // Identity
    uint32_t magic;            // FJMESH_MAGIC
    uint32_t version;          // FJMESH_VERSION
    uint32_t flags;            // reserved, 0 for now

    // Counts
    uint32_t vertex_count;
    uint32_t index_count;
    uint32_t submesh_count;    // total submeshes across all LODs
    uint32_t meshlet_count;
    uint32_t meshlet_vertex_count;       // total entries in meshlet_vertices[]
    uint32_t meshlet_triangle_byte_count; // total bytes in meshlet_triangles[]
    uint32_t collision_vertex_count;
    uint32_t collision_index_count;

    // Meshlet build parameters (reject files built with different limits)
    uint32_t max_meshlet_vertices;
    uint32_t max_meshlet_triangles;

    // LOD
    uint32_t lod_count;        // number of LOD levels (1 = LOD 0 only, v1 compat)

    // Bounding box
    glm::vec3 aabb_min;
    glm::vec3 aabb_max;

    // Section byte offsets from start of file
    uint64_t vertices_offset;
    uint64_t indices_offset;
    uint64_t submeshes_offset;
    uint64_t meshlets_offset;
    uint64_t meshlet_bounds_offset;
    uint64_t meshlet_vertices_offset;
    uint64_t meshlet_triangles_offset;
    uint64_t collision_vertices_offset;
    uint64_t collision_indices_offset;
    uint64_t lod_levels_offset;   // FjMeshLodLevel[lod_count]
};

// Layout validation — if any of these fire, the binary format is incompatible
static_assert(sizeof(FjMeshSubmesh) == 24, "FjMeshSubmesh layout changed");
static_assert(sizeof(FjMeshLodLevel) == 16, "FjMeshLodLevel layout changed");

} // namespace fjell
