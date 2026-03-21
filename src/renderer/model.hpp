#pragma once

#include "renderer/fjmesh.hpp"
#include "renderer/frustum.hpp"
#include "renderer/mega_buffer.hpp"
#include "renderer/mesh.hpp"
#include "renderer/meshlet.hpp"
#include "renderer/meshlet_buffer.hpp"
#include "renderer/texture.hpp"

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
    AABB aabb;
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

private:
    std::vector<std::unique_ptr<Mesh>> meshes_;
    std::vector<std::unique_ptr<Texture>> textures_;
    std::vector<glm::vec3> collision_vertices_;
    std::vector<uint32_t> collision_indices_;
    AABB local_aabb_;
    std::vector<MegaMeshEntry> mega_entries_;
    std::vector<MeshletBuildResult> meshlet_results_;
};

} // namespace fjell
