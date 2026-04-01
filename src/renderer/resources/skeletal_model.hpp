#pragma once

#include "animation/skeleton.hpp"
#include "renderer/gpu/buffer.hpp"
#include "renderer/frustum.hpp"
#include "renderer/resources/vertex.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fjell {

// CPU-side data parsed from a .fjskel or skinned .fjmesh file.
struct FjSkinnedMeshData {
    std::vector<SkinnedVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<glm::vec3> collision_vertices;
    std::vector<uint32_t> collision_indices;
    uint32_t joint_count{0};
    AABB aabb;
    // Skeleton data (populated when loading .fjskel)
    std::vector<FjSkelJoint> joints;
};

// A skinned mesh with per-model VBO/IBO (not MegaBuffer).
// Used for skeletal animation — different vertex stride (SkinnedVertex, 92 bytes)
// and separate draw path from static Model.
// When loaded from .fjskel, also owns the embedded Skeleton.
class SkeletalModel {
public:
    SkeletalModel(VkDevice device, VkPhysicalDevice physical_device,
                  VkCommandPool command_pool, VkQueue transfer_queue,
                  FjSkinnedMeshData&& data);

    // Parse a .fjskel file (mesh + embedded skeleton).
    [[nodiscard]] static FjSkinnedMeshData load_fjskel(const std::string& path);

    void bind(VkCommandBuffer cmd) const;
    void draw(VkCommandBuffer cmd) const;

    [[nodiscard]] uint32_t index_count() const { return index_count_; }
    [[nodiscard]] uint32_t joint_count() const { return joint_count_; }
    [[nodiscard]] const AABB& local_aabb() const { return local_aabb_; }
    [[nodiscard]] float bounding_radius() const { return bounding_radius_; }

    // Embedded skeleton (non-null when loaded from .fjskel).
    [[nodiscard]] const Skeleton* skeleton() const { return skeleton_.get(); }
    [[nodiscard]] const std::shared_ptr<Skeleton>& skeleton_shared() const { return skeleton_; }

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
    std::shared_ptr<Skeleton> skeleton_;
};

} // namespace fjell
