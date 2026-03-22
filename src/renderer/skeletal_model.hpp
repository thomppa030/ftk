#pragma once

#include "renderer/buffer.hpp"
#include "renderer/frustum.hpp"
#include "renderer/vertex.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

namespace fjell {

// CPU-side data parsed from a skinned .fjmesh file.
struct FjSkinnedMeshData {
    std::vector<SkinnedVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<glm::vec3> collision_vertices;
    std::vector<uint32_t> collision_indices;
    uint32_t joint_count{0};
    AABB aabb;
};

// A skinned mesh with per-model VBO/IBO (not MegaBuffer).
// Used for skeletal animation — different vertex stride (SkinnedVertex, 92 bytes)
// and separate draw path from static Model.
class SkeletalModel {
public:
    SkeletalModel(VkDevice device, VkPhysicalDevice physical_device,
                  VkCommandPool command_pool, VkQueue transfer_queue,
                  FjSkinnedMeshData&& data);

    // Parse a skinned .fjmesh file (CPU-side only, no GPU upload).
    [[nodiscard]] static FjSkinnedMeshData load_fjmesh(const std::string& path);

    void bind(VkCommandBuffer cmd) const;
    void draw(VkCommandBuffer cmd) const;

    [[nodiscard]] uint32_t index_count() const { return index_count_; }
    [[nodiscard]] uint32_t joint_count() const { return joint_count_; }
    [[nodiscard]] const AABB& local_aabb() const { return local_aabb_; }
    [[nodiscard]] float bounding_radius() const { return bounding_radius_; }

    [[nodiscard]] const std::vector<glm::vec3>& collision_vertices() const {
        return collision_vertices_;
    }
    [[nodiscard]] const std::vector<uint32_t>& collision_indices() const {
        return collision_indices_;
    }

private:
    Buffer vertex_buffer_;
    Buffer index_buffer_;
    uint32_t index_count_{0};
    uint32_t joint_count_{0};
    AABB local_aabb_;
    float bounding_radius_{0.0f};
    std::vector<glm::vec3> collision_vertices_;
    std::vector<uint32_t> collision_indices_;
};

} // namespace fjell
