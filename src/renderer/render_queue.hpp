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

    // Instanced draws: local-space transforms in owning node's space.
    // nullptr = single-instance draw (normal path).
    const std::vector<glm::mat4>* instance_transforms{nullptr};

    // Spline-mesh draws: per-instance segment records (endpoints, tangents,
    // forward axis, half span). Size must match instance_transforms->size()
    // when both are set. nullptr = not a spline-mesh draw.
    // The records are in world space.
    const std::vector<struct GpuSplineSegment>* spline_segments{nullptr};
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
