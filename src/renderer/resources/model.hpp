#pragma once

#include "physics/collision_shape.hpp"
#include "renderer/resources/fjmesh.hpp"
#include "renderer/frustum.hpp"
#include "renderer/resources/mega_buffer.hpp"
#include "renderer/resources/meshlet.hpp"
#include "renderer/resources/meshlet_buffer.hpp"
#include "renderer/resources/texture.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fjell {

struct PrimitiveMeshData;

// CPU-side mesh data loaded from a .fjmesh file, ready for GPU upload.
struct FjMeshData {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<FjMeshSubmesh> submeshes;
    std::vector<Meshlet> meshlets;
    std::vector<MeshletBounds> bounds;
    std::vector<uint32_t> meshlet_vertices;
    std::vector<uint8_t> meshlet_triangles;
    std::vector<glm::vec3> collision_vertices;
    std::vector<uint32_t> collision_indices;

    // Collision geometry as captured from the source mesh, before the
    // collision source was applied. Empty for files written before v7, which
    // kept only the derived result.
    std::vector<glm::vec3> collision_full_vertices;
    std::vector<uint32_t> collision_full_indices;

    // The shapes this mesh collides as. Empty for files older than v8, which
    // the loader fills with one triangle-mesh primitive covering everything.
    std::vector<CollisionPrimitive> collision_primitives;

    std::vector<FjMeshLodLevel> lod_levels;
    AABB aabb;

    // SDF (v4+)
    std::vector<int16_t> sdf_data;
    uint32_t sdf_resolution{0};
    glm::vec3 sdf_min{0.0f};
    glm::vec3 sdf_max{0.0f};

    // Authored settings (v5+)
    MeshAuthoredData authored;

    // The file format version this was read from.
    uint32_t version{0};
};

// Runtime LOD level: maps to a range of MegaMeshEntries
struct LodLevel {
    uint32_t first_mega_entry{0};
    uint32_t mega_entry_count{0};
    float max_screen_size{1.0f};
};

class Model {
public:
    // Every constructor adds the model's geometry to `mega` and its
    // meshlets to `meshlets`, one mega entry per submesh of each LOD.

    // Load from glTF/GLB file (existing path); `gpu` makes the file's own
    // textures.
    Model(gpu::Device& gpu, const std::string& path, MegaBuffer& mega, MeshletBuffer& meshlets);

    // Create from primitive mesh data (cube/plane/sphere)
    Model(const PrimitiveMeshData& data, MegaBuffer& mega, MeshletBuffer& meshlets);

    // Create from pre-loaded .fjmesh data (no parsing, no meshlet building)
    Model(FjMeshData&& data, MegaBuffer& mega, MeshletBuffer& meshlets);

    // Load .fjmesh binary file into CPU-side data (no GPU upload)
    [[nodiscard]] static FjMeshData load_fjmesh(const std::string& path);


    [[nodiscard]] const std::vector<std::unique_ptr<Texture>>& textures() const {
        return textures_;
    }
    /// Indices across every mega entry, of every LOD.
    [[nodiscard]] uint32_t total_indices() const;

    // Raw collision geometry as stored in the file. Prefer
    // collision_mesh_of() in physics/collision_shape.hpp — it carries the
    // source alongside the geometry, which is what callers actually need.
    [[nodiscard]] const std::vector<glm::vec3>& collision_vertices() const {
        return collision_vertices_;
    }
    [[nodiscard]] const std::vector<uint32_t>& collision_indices() const {
        return collision_indices_;
    }
    [[nodiscard]] FjMeshCollisionSource collision_source() const {
        return authored_.collision_source;
    }

    // The geometry the collision above was derived from, kept so the source
    // can be changed without reimporting. Empty for meshes written before v7:
    // those hold only the derived result, and deriving again from it would
    // compound the loss.
    [[nodiscard]] const std::vector<glm::vec3>& collision_full_vertices() const {
        return collision_full_vertices_;
    }
    [[nodiscard]] const std::vector<uint32_t>& collision_full_indices() const {
        return collision_full_indices_;
    }
    [[nodiscard]] bool has_full_collision() const {
        return !collision_full_vertices_.empty();
    }

    // The shapes this mesh collides as, in its local space. Never empty for a
    // mesh with collision geometry: a file that authored no shapes collides as
    // one triangle mesh covering all of it.
    [[nodiscard]] const std::vector<CollisionPrimitive>& collision_primitives() const {
        return collision_primitives_;
    }

    // Line pairs outlining one primitive's geometry, for overlays. Built on
    // first use and kept for the model's lifetime, which is safe because the
    // collision data above is fixed at construction. Empty for the authored
    // kinds, which have no geometry to outline. Prefer the free function of
    // the same name in physics/collision_shape.hpp.
    [[nodiscard]] const std::vector<glm::vec3>& collision_primitive_edges(size_t index) const {
        static const std::vector<glm::vec3> none;
        if (index >= collision_primitives_.size()) return none;
        if (collision_edges_.size() != collision_primitives_.size()) {
            collision_edges_.resize(collision_primitives_.size());
        }
        auto& slot = collision_edges_[index];
        if (!slot) {
            slot = fjell::collision_primitive_edges(collision_primitives_[index],
                                                    collision_mesh_of(*this));
        }
        return *slot;
    }

    // The .fjmesh version this was loaded from, so the editor can tell whether
    // the file has room for what is being authored. Zero for meshes not loaded
    // from a file at all.
    [[nodiscard]] uint32_t authored_version() const { return authored_version_; }

    [[nodiscard]] const AABB& local_aabb() const { return local_aabb_; }

    [[nodiscard]] const std::vector<MegaMeshEntry>& mega_entries() const { return mega_entries_; }
    [[nodiscard]] uint32_t mega_entry_count() const {
        return static_cast<uint32_t>(mega_entries_.size());
    }

    [[nodiscard]] const std::vector<MeshletBuildResult>& meshlet_results() const { return meshlet_results_; }
    [[nodiscard]] uint32_t total_meshlet_count() const;

    // LOD selection: returns the LOD index for a given screen-space bounding sphere size.
    // screen_size is the fraction of screen height covered by the object's bounding sphere.
    // Returns lod_count() for "distance culled" (object too small to draw).
    [[nodiscard]] uint32_t select_lod(float screen_size) const;

    // Squared-distance LOD selection (avoids sqrt).
    // r_proj_sq = (radius * scale * proj_scale)^2
    // dist_sq   = squared distance to camera
    // Compares r_proj_sq / dist_sq against threshold^2.
    [[nodiscard]] uint32_t select_lod_sq(float r_proj_sq, float dist_sq) const;

    [[nodiscard]] uint32_t lod_count() const {
        return static_cast<uint32_t>(lod_levels_.size());
    }
    [[nodiscard]] const std::vector<LodLevel>& lod_levels() const { return lod_levels_; }

    // Bounding sphere radius (computed from AABB)
    [[nodiscard]] float bounding_radius() const { return bounding_radius_; }

    // Authored settings loaded from the .fjmesh (v5+). Empty for older files.
    [[nodiscard]] const MeshAuthoredData& authored() const { return authored_; }

    // Material slot per submesh, indexed the same way LodLevel indexes mega
    // entries. Empty for meshes built without submesh data (glTF, primitives),
    // in which case every entry uses the object's single material.
    [[nodiscard]] const std::vector<uint32_t>& entry_material_slots() const {
        return entry_material_slots_;
    }
    // Whether a draw limited to `slot` covers mega entry `entry`. UINT32_MAX
    // is the single-material draw and covers every entry.
    [[nodiscard]] bool entry_uses_slot(uint32_t entry, uint32_t slot) const {
        if (slot == UINT32_MAX) return true;
        return entry < entry_material_slots_.size() && entry_material_slots_[entry] == slot;
    }
    // Calls `visit(entry)` for each mega entry a draw of `slot` at `lod`
    // covers: the LOD's entries whose submesh uses the slot. Nothing for a
    // LOD past the last, which is how select_lod() says "too small to draw".
    template <typename Visit>
    void for_each_entry(uint32_t lod, uint32_t slot, Visit&& visit) const {
        if (lod >= lod_levels_.size()) return;
        const LodLevel& level = lod_levels_[lod];
        for (uint32_t e = level.first_mega_entry; e < level.first_mega_entry + level.mega_entry_count; ++e) {
            if (entry_uses_slot(e, slot)) visit(mega_entries_[e]);
        }
    }

    [[nodiscard]] uint32_t material_slot_count() const {
        return static_cast<uint32_t>(authored_.material_slots.size());
    }

    // SDF access
    [[nodiscard]] bool has_sdf() const { return sdf_resolution_ > 0; }
    [[nodiscard]] uint32_t sdf_grid_resolution() const { return sdf_resolution_; }
    [[nodiscard]] const std::vector<int16_t>& sdf_grid_data() const { return sdf_data_; }
    [[nodiscard]] const glm::vec3& sdf_volume_min() const { return sdf_min_; }
    [[nodiscard]] const glm::vec3& sdf_volume_max() const { return sdf_max_; }

private:
    /// Adds one submesh: its vertices and indices as a mega entry, with the
    /// entry's meshlets when it has any.
    void add_entry(MegaBuffer& mega, MeshletBuffer& meshlets, const std::vector<Vertex>& vertices,
                   const std::vector<uint32_t>& indices, const MeshletBuildResult& meshlet_result);

    std::vector<std::unique_ptr<Texture>> textures_;
    std::vector<glm::vec3> collision_vertices_;
    std::vector<uint32_t> collision_indices_;
    std::vector<glm::vec3> collision_full_vertices_;
    std::vector<uint32_t> collision_full_indices_;
    std::vector<CollisionPrimitive> collision_primitives_;
    // One entry per primitive, filled by collision_primitive_edges().
    mutable std::vector<std::optional<std::vector<glm::vec3>>> collision_edges_;
    uint32_t authored_version_{0};
    AABB local_aabb_;
    std::vector<MegaMeshEntry> mega_entries_;
    std::vector<MeshletBuildResult> meshlet_results_;
    std::vector<LodLevel> lod_levels_;
    float bounding_radius_{0.0f};

    MeshAuthoredData authored_;
    std::vector<uint32_t> entry_material_slots_;

    // SDF
    std::vector<int16_t> sdf_data_;
    uint32_t sdf_resolution_{0};
    glm::vec3 sdf_min_{0.0f};
    glm::vec3 sdf_max_{0.0f};
};

} // namespace fjell
