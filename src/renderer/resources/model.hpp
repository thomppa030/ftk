#pragma once

#include "renderer/resources/fjmesh.hpp"
#include "renderer/frustum.hpp"
#include "renderer/resources/mega_buffer.hpp"
#include "renderer/resources/mesh.hpp"
#include "renderer/resources/meshlet.hpp"
#include "renderer/resources/meshlet_buffer.hpp"
#include "renderer/resources/texture.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
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
    std::vector<FjMeshLodLevel> lod_levels;
    AABB aabb;

    // SDF (v4+)
    std::vector<int16_t> sdf_data;
    uint32_t sdf_resolution{0};
    glm::vec3 sdf_min{0.0f};
    glm::vec3 sdf_max{0.0f};
};

// Runtime LOD level: maps to a range of MegaMeshEntries
struct LodLevel {
    uint32_t first_mega_entry{0};
    uint32_t mega_entry_count{0};
    float max_screen_size{1.0f};
};

class Model {
public:
    // Load from glTF/GLB file (existing path)
    Model(VkDevice device, VkPhysicalDevice physical_device,
          VkCommandPool command_pool, VkQueue queue,
          const std::string& path, MegaBuffer* mega = nullptr,
          MeshletBuffer* meshlet_buf = nullptr);

    // Create from primitive mesh data (cube/plane/sphere)
    Model(VkDevice device, VkPhysicalDevice physical_device,
          VkCommandPool command_pool, VkQueue queue,
          const PrimitiveMeshData& data, MegaBuffer* mega = nullptr,
          MeshletBuffer* meshlet_buf = nullptr);

    // Create from pre-loaded .fjmesh data (no parsing, no meshlet building)
    Model(VkDevice device, VkPhysicalDevice physical_device,
          VkCommandPool command_pool, VkQueue queue,
          FjMeshData&& data, MegaBuffer* mega = nullptr,
          MeshletBuffer* meshlet_buf = nullptr);

    // Load .fjmesh binary file into CPU-side data (no GPU upload)
    [[nodiscard]] static FjMeshData load_fjmesh(const std::string& path);

    void draw(VkCommandBuffer cmd) const;
    void draw(VkCommandBuffer cmd, uint32_t first_instance) const;

    [[nodiscard]] const std::vector<std::unique_ptr<Texture>>& textures() const {
        return textures_;
    }
    [[nodiscard]] uint32_t mesh_count() const {
        return static_cast<uint32_t>(meshes_.size());
    }
    [[nodiscard]] uint32_t total_indices() const;

    [[nodiscard]] const std::vector<glm::vec3>& collision_vertices() const {
        return collision_vertices_;
    }
    [[nodiscard]] const std::vector<uint32_t>& collision_indices() const {
        return collision_indices_;
    }

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

    // SDF access
    [[nodiscard]] bool has_sdf() const { return sdf_resolution_ > 0; }
    [[nodiscard]] uint32_t sdf_grid_resolution() const { return sdf_resolution_; }
    [[nodiscard]] const std::vector<int16_t>& sdf_grid_data() const { return sdf_data_; }
    [[nodiscard]] const glm::vec3& sdf_volume_min() const { return sdf_min_; }
    [[nodiscard]] const glm::vec3& sdf_volume_max() const { return sdf_max_; }

private:
    std::vector<std::unique_ptr<Mesh>> meshes_;
    std::vector<std::unique_ptr<Texture>> textures_;
    std::vector<glm::vec3> collision_vertices_;
    std::vector<uint32_t> collision_indices_;
    AABB local_aabb_;
    std::vector<MegaMeshEntry> mega_entries_;
    std::vector<MeshletBuildResult> meshlet_results_;
    std::vector<LodLevel> lod_levels_;
    float bounding_radius_{0.0f};

    // SDF
    std::vector<int16_t> sdf_data_;
    uint32_t sdf_resolution_{0};
    glm::vec3 sdf_min_{0.0f};
    glm::vec3 sdf_max_{0.0f};
};

} // namespace fjell
