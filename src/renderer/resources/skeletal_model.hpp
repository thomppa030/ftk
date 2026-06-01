#pragma once

#include "animation/skeleton.hpp"
#include "renderer/gpu/buffer.hpp"
#include "renderer/frustum.hpp"
#include "renderer/resources/meshlet.hpp"
#include "renderer/resources/meshlet_buffer.hpp"
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
class MeshletBuffer;

class SkeletalModel {
public:
    SkeletalModel(VmaAllocator allocator, VkDevice device,
                  VkCommandPool command_pool, VkQueue transfer_queue,
                  FjSkinnedMeshData&& data,
                  MeshletBuffer* meshlet_buf = nullptr);

    // Parse a .fjskel file (mesh + embedded skeleton).
    [[nodiscard]] static FjSkinnedMeshData load_fjskel(const std::string& path);

    void bind(VkCommandBuffer cmd) const;
    void draw(VkCommandBuffer cmd) const;

    [[nodiscard]] uint32_t index_count() const { return index_count_; }
    [[nodiscard]] uint32_t joint_count() const { return joint_count_; }
    [[nodiscard]] const AABB& local_aabb() const { return local_aabb_; }
    [[nodiscard]] float bounding_radius() const { return bounding_radius_; }

    // CPU-side meshlet build for the GPU-driven skinned mesh-shader path.
    // One MeshletBuildResult covers the whole mesh (single LOD for now;
    // skinned LOD chains would need a per-LOD bone-weight remap that
    // hasn't been built yet).
    [[nodiscard]] const MeshletBuildResult& meshlets() const { return meshlets_; }

    // GPU range inside the shared MeshletBuffer (populated when meshlet_buf
    // was passed at construction). vertex_offset is unused on the skinned
    // path — the mesh shader fetches from vertex_buffer_ at zero base.
    [[nodiscard]] const MeshletRange& meshlet_range() const { return meshlet_range_; }

    [[nodiscard]] VkBuffer vertex_buffer_handle() const { return vertex_buffer_.handle(); }
    [[nodiscard]] VkDeviceSize vertex_buffer_size() const { return vertex_buffer_size_; }

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
    MeshletBuildResult meshlets_;
    MeshletRange meshlet_range_;
    VkDeviceSize vertex_buffer_size_{0};
};

} // namespace fjell
