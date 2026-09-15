#pragma once

// .fjmesh binary format — Fjell engine internal mesh format.
// Shared between the engine runtime and the fjimport tool.
// No engine dependencies — standard library and <glm/glm.hpp> only.

#include <glm/glm.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace fjell {

static constexpr uint32_t FJMESH_MAGIC   = 0x484D4A46; // "FJMH" little-endian
static constexpr uint32_t FJMESH_VERSION = 9;

// Files from this version on have every header field and section the current
// layout expects, so the writers can rewrite them in place. Older files need
// a reimport.
static constexpr uint32_t FJMESH_LAYOUT_VERSION = 8;

// Files before this version store the SDF with the opposite sign, positive
// inside the mesh. Readers flip it on load; rewriters flip it and stamp the
// current version.
static constexpr uint32_t FJMESH_SDF_SIGN_VERSION = 9;

inline void flip_sdf_sign(int16_t* data, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        data[i] = data[i] == INT16_MIN ? INT16_MAX : static_cast<int16_t>(-data[i]);
    }
}

static constexpr uint32_t FJMESH_FLAG_SKINNED = 1; // vertices are SkinnedVertex (92 bytes)

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

// Where a mesh's collision geometry comes from. The collision section holds
// the resulting geometry; this records which kind it is so the runtime can
// pick the matching collision shape and the geometry can be regenerated.
enum class FjMeshCollisionSource : uint32_t {
    render_mesh = 0,   // verbatim copy of the render geometry (triangle mesh, static only)
    convex_hull = 1,   // hull of the render geometry — works on dynamic bodies
    simplified = 2,    // decimated triangle mesh, static only
    custom_mesh = 3,   // a separate .fjmesh, referenced by name-table path
};

// A material slot on the mesh. `name` is the material name from the source
// file and is the key used to carry assignments across a reimport; `path` is
// the assigned .fjmat, relative to the project root. Both index the name table.
struct FjMeshMaterialSlot {
    uint32_t name_offset;
    uint32_t name_length;
    uint32_t path_offset;
    uint32_t path_length;
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

    // v3+ fields (appended at the end for backward compatibility)
    uint32_t joint_count;      // 0 for static meshes, >0 for skinned
    uint32_t pad_v3;           // alignment

    // v4+ fields — per-mesh SDF (signed distance field)
    uint32_t sdf_resolution;   // grid cells per axis (0 = no SDF data)
    uint32_t sdf_pad;
    glm::vec3 sdf_min;         // local-space AABB of SDF volume (with margin)
    float sdf_pad2;
    glm::vec3 sdf_max;
    float sdf_pad3;
    uint64_t sdf_data_offset;  // int16_t[res*res*res] SDF voxel data

    // v5+ fields — authored data, preserved across reimport
    uint32_t material_slot_count;
    uint32_t collision_source;          // FjMeshCollisionSource
    float    collision_simplify_ratio;  // target ratio for `simplified`
    uint32_t source_name_offset;        // source file this was imported from
    uint32_t source_name_length;
    uint32_t collision_mesh_offset;     // .fjmesh path for `custom_mesh`
    uint32_t collision_mesh_length;
    uint32_t pad_v5;
    uint64_t source_modified_time;      // source mtime at import, seconds since epoch
    uint64_t material_slots_offset;     // FjMeshMaterialSlot[material_slot_count]
    uint64_t name_table_offset;         // packed strings, no null terminators
    uint64_t name_table_size;

    // v6+ fields — asset identity, so a move doesn't orphan the mesh
    uint32_t asset_uuid_offset;         // 32 hex chars, into the name table
    uint32_t asset_uuid_length;

    // v7+ fields — the collision geometry as captured from the source mesh,
    // before collision_source was applied to it. The derived geometry above is
    // what the runtime collides against; this is what it was derived from, so
    // the source can be changed without reimporting. When the source is
    // render_mesh the two are identical and these offsets point at the same
    // bytes rather than storing a second copy.
    uint32_t collision_full_vertex_count;
    uint32_t collision_full_index_count;
    uint64_t collision_full_vertices_offset;
    uint64_t collision_full_indices_offset;

    // v8+ fields — the collision shape list. A mesh collides as the shapes in
    // this array rather than as one blob derived from its geometry, so a crate
    // can be a box and a concave prop can be several hulls. Zero entries means
    // the whole collision section is one implicit triangle mesh, which is what
    // every file written before v8 holds.
    uint32_t collision_primitive_count;
    uint32_t pad_v8;
    uint64_t collision_primitives_offset;
};

// On-disk form of CollisionPrimitive. Mirrors the engine struct field for
// field; kept separate so the runtime type can gain helpers without changing
// the format. The quaternion is stored x,y,z,w.
struct FjMeshCollisionPrimitive {
    uint32_t kind;
    float position[3];
    float rotation[4];
    float extents[3];
    uint32_t first_vertex;
    uint32_t vertex_count;
    uint32_t first_index;
    uint32_t index_count;
};

// ── .fjskel format ──────────────────────────────────────────────────────
// Skeletal mesh = skinned geometry + embedded skeleton.
// File layout: FjSkelHeader, then mesh data sections (same as skinned .fjmesh),
// then skeleton joints section, then joint name table.

static constexpr uint32_t FJSKEL_MAGIC   = 0x4C4B5346; // "FSKL" little-endian
static constexpr uint32_t FJSKEL_VERSION = 2;

// Binary joint for the embedded skeleton section.
struct FjSkelJointBin {
    int32_t parent;            // -1 for root
    float translation[3];
    float rotation[4];         // x, y, z, w
    float scale[3];
    uint32_t name_offset;      // byte offset into the name table (from name_table_offset)
    uint32_t name_length;      // byte length of the name (no null terminator)
};

struct FjSkelHeader {
    // Identity
    uint32_t magic;            // FJSKEL_MAGIC
    uint32_t version;          // FJSKEL_VERSION

    // Mesh geometry (same data as skinned .fjmesh)
    uint32_t vertex_count;
    uint32_t index_count;
    uint32_t joint_count;
    uint32_t collision_vertex_count;
    uint32_t collision_index_count;
    uint32_t pad0;

    // Bounding box
    glm::vec3 aabb_min;
    glm::vec3 aabb_max;

    // Section byte offsets from start of file
    uint64_t vertices_offset;           // SkinnedVertex[vertex_count]
    uint64_t indices_offset;            // uint32_t[index_count]
    uint64_t collision_vertices_offset; // vec3[collision_vertex_count]
    uint64_t collision_indices_offset;  // uint32_t[collision_index_count]
    uint64_t joints_offset;             // FjSkelJointBin[joint_count]
    uint64_t name_table_offset;         // packed joint name strings
    uint64_t name_table_size;           // total bytes in name table

    // v2+ fields — collision shapes carried by the skeleton's bones. Authored
    // on the skeleton so every character using it inherits them, the same way
    // mesh collision is authored on the .fjmesh.
    uint32_t collider_count;
    uint32_t pad_v2;
    uint64_t colliders_offset;          // FjSkelCollider[collider_count]
};

// On-disk form of a bone collider. The bone is named rather than indexed so a
// re-export with a different joint order cannot silently move every shape onto
// the wrong limb; the name indexes the same table the joints use.
struct FjSkelCollider {
    uint32_t kind;              // CollisionPrimitiveKind
    uint32_t bone_name_offset;
    uint32_t bone_name_length;
    uint32_t pad;
    float position[3];
    float rotation[4];          // x, y, z, w
    float extents[3];
};

// ── Authored mesh settings (v5) ─────────────────────────────────────────
// The subset of a .fjmesh that a user edits rather than the importer
// generating. Kept separate from the geometry so a reimport can carry it
// across: read it from the old file, hand it to the writer for the new one.

struct MeshMaterialSlot {
    std::string name;   // material name from the source file — the reimport key
    std::string path;   // assigned .fjmat, relative to the project root
};

struct MeshAuthoredData {
    std::vector<MeshMaterialSlot> material_slots;
    FjMeshCollisionSource collision_source{FjMeshCollisionSource::render_mesh};
    float collision_simplify_ratio{0.1F};
    std::string collision_mesh;      // .fjmesh proxy, for custom_mesh
    std::string source_file;         // what this mesh was imported from
    uint64_t source_modified_time{0};
    std::string asset_uuid;          // identity, so a move doesn't orphan it

    [[nodiscard]] bool empty() const {
        return material_slots.empty() && collision_mesh.empty() && source_file.empty() &&
               asset_uuid.empty() &&
               collision_source == FjMeshCollisionSource::render_mesh;
    }
};

// Packs strings into a name table, returning (offset, length) pairs. Repeated
// strings share one entry — material paths in particular tend to repeat.
class NameTableBuilder {
public:
    std::pair<uint32_t, uint32_t> add(const std::string& s) {
        if (s.empty()) return {0, 0};
        auto it = interned_.find(s);
        if (it != interned_.end()) return it->second;

        std::pair<uint32_t, uint32_t> entry{static_cast<uint32_t>(data_.size()),
                                            static_cast<uint32_t>(s.size())};
        data_.insert(data_.end(), s.begin(), s.end());
        interned_.emplace(s, entry);
        return entry;
    }

    [[nodiscard]] const std::vector<char>& data() const { return data_; }
    [[nodiscard]] size_t size() const { return data_.size(); }

private:
    std::vector<char> data_;
    std::map<std::string, std::pair<uint32_t, uint32_t>> interned_;
};

// Reads one string out of a loaded name table. Out-of-range offsets yield an
// empty string rather than throwing — a truncated table shouldn't take down
// the whole mesh load.
[[nodiscard]] inline std::string read_name(const std::vector<char>& table,
                                           uint32_t offset, uint32_t length) {
    // The bound is checked in 64-bit so a corrupt offset+length cannot wrap and
    // pass — a garbage length must never turn into a multi-gigabyte allocation.
    if (length == 0) return {};
    if (static_cast<uint64_t>(offset) + length > table.size()) return {};
    return std::string(table.data() + offset, length);
}

// Layout validation — if any of these fire, the binary format is incompatible
static_assert(sizeof(FjMeshSubmesh) == 24, "FjMeshSubmesh layout changed");
static_assert(sizeof(FjMeshLodLevel) == 16, "FjMeshLodLevel layout changed");
static_assert(sizeof(FjMeshMaterialSlot) == 16, "FjMeshMaterialSlot layout changed");

} // namespace fjell
