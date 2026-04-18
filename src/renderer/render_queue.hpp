#pragma once

#include "renderer/frustum.hpp"
#include "renderer/uniform_types.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace fjell {

class Model;
class SkeletalModel;
struct RenderStats;

struct DrawCommand {
    uint64_t sort_key{0};
    glm::mat4 world_matrix{1.0f};
    glm::mat4 prev_world_matrix{1.0f}; // previous frame (for motion vectors)
    PbrPushConstants push{};
    Model* model{nullptr};
    SkeletalModel* skeletal_model{nullptr};
    AABB world_aabb;           // pre-computed in frustum cull, reused in indirect batch build
    float camera_distance{0.0f};
    uint32_t pipeline_id{0};
    uint32_t node_id{0};
    bool transparent{false};
    bool skinned{false};

    // Instanced draws: pointer to a vector of WORLD-SPACE per-instance
    // matrices (owned by the InstancedMeshComponent and refreshed only when
    // the component is dirty or its node moves). nullptr = single-instance.
    const std::vector<glm::mat4>* instance_transforms_world{nullptr};

    // Per-component GPU buffer addresses for BDA-capable shaders. Zero =
    // BDA path unavailable, caller falls back to the shared SSBO.
    VkDeviceAddress instance_transforms_addr{0};
    VkDeviceAddress visible_transforms_addr{0};
    VkDeviceAddress prev_instance_transforms_addr{0};
};

class RenderQueue {
public:
    void clear();
    void reserve(size_t count);
    void push(const DrawCommand& cmd);
    void sort(const glm::vec3& camera_pos);
    void execute(VkCommandBuffer cmd, VkPipelineLayout layout,
                 RenderStats* stats = nullptr) const;
    void execute_transparent(VkCommandBuffer cmd, VkPipelineLayout layout,
                             RenderStats* stats = nullptr) const;

    [[nodiscard]] bool empty() const { return opaque_.empty() && transparent_.empty(); }
    [[nodiscard]] bool has_transparent() const { return !transparent_.empty(); }
    [[nodiscard]] const std::vector<DrawCommand>& commands() const { return opaque_; }
    [[nodiscard]] const std::vector<DrawCommand>& transparent_commands() const { return transparent_; }

    static uint64_t make_sort_key(uint32_t pipeline_id, uint32_t material_id, uint32_t mesh_id,
                                  bool skinned = false);

private:
    std::vector<DrawCommand> opaque_;
    std::vector<DrawCommand> transparent_;
};

} // namespace fjell
