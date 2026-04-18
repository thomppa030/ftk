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
    uint32_t first_instance{0};          // offset into the shared InstanceTransforms SSBO
                                          // (legacy path; being replaced by BDA fields below)

    // Buffer device addresses for per-component instance data. Valid only when
    // instance_count > 1. Used by BDA-capable shaders; the legacy SSBO path
    // still reads via first_instance. Currently unread — shader migration
    // happens one file at a time in later steps.
    uint64_t instances_addr{0};          // CPU-uploaded world transforms (this frame)
    uint64_t visible_addr{0};            // post-InstanceCullPass compacted transforms
    uint64_t prev_instances_addr{0};     // last frame's transforms (motion vectors)
    uint64_t _pad_end{0};                // pad to 16-byte alignment
};

static_assert(sizeof(GpuDrawData) == 192, "GpuDrawData must be 192 bytes for std430");

// One per instanced draw (each mega-entry × each distinct instanced DrawCommand).
// Consumed by InstanceCullPass to cull instances against the view frustum and
// compact the indirect command's instanceCount in-place.
struct GpuInstancedDraw {
    glm::vec4 local_aabb_min;        // model local AABB min (.w unused)
    glm::vec4 local_aabb_max;        // model local AABB max (.w unused)
    uint32_t first_instance;         // offset into InstanceTransforms
    uint32_t instance_count;         // source instance count (pre-cull)
    uint32_t indirect_cmd_index;     // which VkDrawIndexedIndirectCommand to patch
    uint32_t _pad;
};
static_assert(sizeof(GpuInstancedDraw) == 48, "GpuInstancedDraw must be 48 bytes for std430");

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
               AnimationSystem* animation_system = nullptr,
               bool is_shadow_batch = false);

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

    [[nodiscard]] VkBuffer visible_instance_buffer(uint32_t frame_index) const {
        return visible_instance_buffers_[frame_index]
                   ? visible_instance_buffers_[frame_index]->handle()
                   : VK_NULL_HANDLE;
    }
    [[nodiscard]] VkDeviceSize visible_instance_size(uint32_t frame_index) const {
        return static_cast<VkDeviceSize>(instance_transform_counts_[frame_index]) * sizeof(glm::mat4);
    }

    [[nodiscard]] VkBuffer instanced_draw_buffer(uint32_t frame_index) const {
        return instanced_draw_buffers_[frame_index]
                   ? instanced_draw_buffers_[frame_index]->handle()
                   : VK_NULL_HANDLE;
    }
    [[nodiscard]] VkDeviceSize instanced_draw_size(uint32_t frame_index) const {
        return static_cast<VkDeviceSize>(instanced_draw_counts_[frame_index]) * sizeof(GpuInstancedDraw);
    }
    [[nodiscard]] uint32_t instanced_draw_count(uint32_t frame_index) const {
        return instanced_draw_counts_[frame_index];
    }

private:
    void ensure_capacity(uint32_t frame_index, uint32_t count);
    void ensure_instance_capacity(uint32_t frame_index, uint32_t count);
    void ensure_instanced_draw_capacity(uint32_t frame_index, uint32_t count);

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

    // Per-draw scratch for instanced LOD bucketing — reused across draws to
    // avoid nested std::vector allocations in the hot loop.
    std::vector<uint8_t> staged_bucket_lods_;
    std::vector<glm::mat4> staged_bucket_worlds_;

    // Per-frame skinned draw data indices (draw_index into GpuDrawData SSBO)
    std::array<std::vector<uint32_t>, MAX_FRAMES_IN_FLIGHT> skinned_draw_indices_;

    // Per-frame packed instance transforms (world-space, concatenated for all instanced draws)
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> instance_transform_buffers_;
    // GPU-written compacted visible instances — vertex path reads this
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> visible_instance_buffers_;
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> instance_transform_capacities_{};
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> instance_transform_counts_{};
    std::vector<glm::mat4> staged_instance_transforms_;

    // Per-frame per-instanced-draw descriptors (local AABB + indirect cmd index)
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> instanced_draw_buffers_;
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> instanced_draw_capacities_{};
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> instanced_draw_counts_{};
    std::vector<GpuInstancedDraw> staged_instanced_draws_;
};

} // namespace fjell
