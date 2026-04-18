#pragma once

#include "renderer/gpu/buffer.hpp"
#include "renderer/pass/cull_pass.hpp"
#include "renderer/renderer_constants.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace fjell {

class AnimationSystem;
struct DrawCommand;
struct MegaMeshEntry;
class Model;

struct GpuDrawData {
    glm::mat4 model;
    glm::mat4 prev_model;           // previous frame's world matrix (for motion vectors)
    uint32_t material_id;
    uint32_t meshlet_offset;
    uint32_t meshlet_count;
    uint32_t vertex_offset;
    uint32_t bone_offset{0xFFFFFFFF};   // offset into bone SSBO (0xFFFFFFFF = not skinned)
    uint32_t bone_count{0};
    uint32_t instance_count{1};          // 1 = single instance; N = instanced
    uint32_t first_instance{0};          // offset into instance transform SSBO
};

static_assert(sizeof(GpuDrawData) == 160, "GpuDrawData must be 160 bytes for std430");

// A contiguous range of indirect draw commands sharing the same pipeline.
struct PipelineDrawRange {
    uint32_t pipeline_id{0};
    uint32_t first_draw{0};   // offset into indirect buffer (in commands)
    uint32_t draw_count{0};
    bool skinned{false};
};

class IndirectBatch {
public:
    IndirectBatch(VkDevice device, VmaAllocator allocator);

    void build(const std::vector<DrawCommand>& opaque_commands,
               const std::vector<DrawCommand>& transparent_commands,
               uint32_t frame_index,
               const glm::mat4& projection = glm::mat4(1.0f),
               const glm::vec3& camera_pos = glm::vec3(0.0f),
               AnimationSystem* animation_system = nullptr);

    void draw(VkCommandBuffer cmd, uint32_t frame_index) const;

    [[nodiscard]] VkBuffer draw_data_buffer(uint32_t frame_index) const {
        return draw_data_buffers_[frame_index]->handle();
    }
    [[nodiscard]] VkDeviceSize draw_data_size(uint32_t frame_index) const {
        return draw_data_counts_[frame_index] * sizeof(GpuDrawData);
    }
    [[nodiscard]] uint32_t draw_data_count(uint32_t frame_index) const {
        return draw_data_counts_[frame_index];
    }
    [[nodiscard]] uint32_t opaque_draw_count(uint32_t frame_index) const {
        return opaque_counts_[frame_index];
    }
    [[nodiscard]] uint32_t total_meshlet_instances(uint32_t frame_index) const {
        return total_meshlet_instances_[frame_index];
    }

    [[nodiscard]] uint32_t total_triangles(uint32_t frame_index) const {
        return total_triangles_[frame_index];
    }

    [[nodiscard]] const std::vector<PipelineDrawRange>& pipeline_ranges(uint32_t frame_index) const {
        return pipeline_ranges_[frame_index];
    }

    [[nodiscard]] VkBuffer indirect_buffer(uint32_t frame_index) const {
        return indirect_buffers_[frame_index]->handle();
    }

    [[nodiscard]] VkBuffer mesh_indirect_buffer(uint32_t frame_index) const {
        return mesh_indirect_buffers_[frame_index] ? mesh_indirect_buffers_[frame_index]->handle() : VK_NULL_HANDLE;
    }
    [[nodiscard]] VkBuffer mesh_count_buffer(uint32_t frame_index) const {
        return mesh_count_buffers_[frame_index] ? mesh_count_buffers_[frame_index]->handle() : VK_NULL_HANDLE;
    }

    [[nodiscard]] VkBuffer pipeline_bin_buffer(uint32_t frame_index) const {
        return pipeline_bin_buffers_[frame_index] ? pipeline_bin_buffers_[frame_index]->handle() : VK_NULL_HANDLE;
    }
    [[nodiscard]] VkDeviceSize pipeline_bin_size(uint32_t frame_index) const {
        return opaque_counts_[frame_index] * sizeof(uint32_t);
    }

    [[nodiscard]] uint32_t draw_meshlet_count(uint32_t frame_index, uint32_t draw_index) const {
        auto* data = static_cast<const GpuDrawData*>(draw_data_buffers_[frame_index]->mapped());
        return data[draw_index].meshlet_count;
    }

    [[nodiscard]] const GpuDrawAABB* aabb_data(uint32_t frame_index) const {
        return aabb_data_[frame_index].empty() ? nullptr : aabb_data_[frame_index].data();
    }

    [[nodiscard]] const std::vector<uint32_t>& skinned_draw_indices(uint32_t frame_index) const {
        return skinned_draw_indices_[frame_index];
    }

    [[nodiscard]] VkBuffer instance_transform_buffer(uint32_t frame_index) const {
        return instance_transform_buffers_[frame_index]
                   ? instance_transform_buffers_[frame_index]->handle()
                   : VK_NULL_HANDLE;
    }
    [[nodiscard]] VkDeviceSize instance_transform_size(uint32_t frame_index) const {
        return static_cast<VkDeviceSize>(instance_transform_counts_[frame_index]) * sizeof(glm::mat4);
    }
    [[nodiscard]] uint32_t instance_transform_count(uint32_t frame_index) const {
        return instance_transform_counts_[frame_index];
    }

private:
    void ensure_capacity(uint32_t frame_index, uint32_t count);
    void ensure_instance_capacity(uint32_t frame_index, uint32_t count);

    VkDevice device_;
    VmaAllocator allocator_;

    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> indirect_buffers_;
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> draw_data_buffers_;
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> mesh_indirect_buffers_;
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> mesh_count_buffers_;
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> capacities_{};
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> opaque_counts_{};
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> draw_data_counts_{};
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> total_triangles_{};
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> total_meshlet_instances_{};
    std::array<std::vector<PipelineDrawRange>, MAX_FRAMES_IN_FLIGHT> pipeline_ranges_;
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> pipeline_bin_buffers_;
    std::array<std::vector<GpuDrawAABB>, MAX_FRAMES_IN_FLIGHT> aabb_data_;

    // CPU-side staging buffers (avoid scattered writes to write-combined GPU memory)
    std::vector<VkDrawIndexedIndirectCommand> staged_indirect_;
    std::vector<GpuDrawData> staged_draw_data_;

    // Per-frame skinned draw data indices (draw_index into GpuDrawData SSBO)
    std::array<std::vector<uint32_t>, MAX_FRAMES_IN_FLIGHT> skinned_draw_indices_;

    // Per-frame packed instance transforms (world-space, concatenated for all instanced draws)
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> instance_transform_buffers_;
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> instance_transform_capacities_{};
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> instance_transform_counts_{};
    std::vector<glm::mat4> staged_instance_transforms_;
};

} // namespace fjell
