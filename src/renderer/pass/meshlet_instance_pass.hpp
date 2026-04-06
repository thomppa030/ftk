#pragma once

#include "renderer/gpu/buffer.hpp"
#include "renderer/gpu/descriptor.hpp"
#include "renderer/renderer_constants.hpp"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>

namespace fjell {

struct GpuMeshletInstance {
    uint32_t meshlet_index;
    uint32_t draw_index;
};
static_assert(sizeof(GpuMeshletInstance) == 8);

class MeshletInstancePass {
public:
    void create(VkDevice device, VmaAllocator allocator,
                VkDescriptorSetLayout global_set_layout);
    void destroy();

    // Resize buffers if total meshlet instance count grew
    void ensure_capacity(uint32_t frame_index, uint32_t max_instances);

    // Dispatch expand pass: scatter draw meshlets into flat instance buffer
    void dispatch_expand(VkCommandBuffer cmd, VkBuffer draw_data_buffer,
                         VkDeviceSize draw_data_size,
                         uint32_t total_draws, uint32_t frame_index);

    // Barrier: expand write → cull read
    void insert_expand_barrier(VkCommandBuffer cmd);

    // Dispatch first cull pass: per-meshlet frustum + occlusion against previous Hi-Z.
    // Outputs visible instances to binned buffer, saves Hi-Z-rejected to culled buffer.
    void dispatch_cull_first(VkCommandBuffer cmd, VkDescriptorSet global_set,
                             VkBuffer draw_data_buffer, VkDeviceSize draw_data_size,
                             VkBuffer bounds_buffer, VkDeviceSize bounds_size,
                             VkBuffer pipeline_bin_buffer, VkDeviceSize pipeline_bin_size,
                             uint32_t num_bins, uint32_t frame_index,
                             VkImageView hiz_view, VkSampler hiz_sampler,
                             uint32_t viewport_width, uint32_t viewport_height,
                             uint32_t hiz_mip_count);

    // Dispatch second cull pass: test previously culled meshlets against NEW Hi-Z.
    // Appends newly visible instances to the same binned output buffer.
    void dispatch_cull_second(VkCommandBuffer cmd, VkDescriptorSet global_set,
                              VkBuffer draw_data_buffer, VkDeviceSize draw_data_size,
                              VkBuffer bounds_buffer, VkDeviceSize bounds_size,
                              VkBuffer pipeline_bin_buffer, VkDeviceSize pipeline_bin_size,
                              uint32_t num_bins, uint32_t frame_index,
                              VkImageView hiz_view, VkSampler hiz_sampler,
                              uint32_t viewport_width, uint32_t viewport_height,
                              uint32_t hiz_mip_count);

    // Barrier: cull write → depth prepass / indirect draw read + task shader read
    void insert_cull_barrier(VkCommandBuffer cmd);

    // Visible instances buffer for binding in meshlet descriptor (set 3 binding 5)
    [[nodiscard]] VkBuffer visible_buffer(uint32_t frame_index) const {
        return visible_buffers_[frame_index] ? visible_buffers_[frame_index]->handle() : VK_NULL_HANDLE;
    }
    [[nodiscard]] VkDeviceSize visible_buffer_size(uint32_t frame_index) const {
        return visible_buffers_[frame_index] ? visible_buffers_[frame_index]->size() : 0;
    }

    // Indirect dispatch command buffer (N * VkDrawMeshTasksIndirectCommandEXT)
    [[nodiscard]] VkBuffer indirect_buffer(uint32_t frame_index) const {
        return indirect_buffers_[frame_index] ? indirect_buffers_[frame_index]->handle() : VK_NULL_HANDLE;
    }

    // Max instances per pipeline bin (segment size in the visible buffer)
    [[nodiscard]] uint32_t segment_size(uint32_t frame_index) const {
        return capacities_[frame_index];
    }

private:
    VkDevice device_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};

    // Expand pass
    VkPipelineLayout expand_layout_{VK_NULL_HANDLE};
    VkPipeline expand_pipeline_{VK_NULL_HANDLE};
    std::unique_ptr<DescriptorSetLayout> expand_set_layout_;
    std::unique_ptr<DescriptorPool> expand_pool_;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> expand_sets_{};

    // Cull pass
    VkPipelineLayout cull_layout_{VK_NULL_HANDLE};
    VkPipeline cull_pipeline_{VK_NULL_HANDLE};
    std::unique_ptr<DescriptorSetLayout> cull_set_layout_;
    std::unique_ptr<DescriptorPool> cull_pool_;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> cull_sets_{};

    void dispatch_cull_internal(VkCommandBuffer cmd, VkDescriptorSet global_set,
                                VkBuffer draw_data_buffer, VkDeviceSize draw_data_size,
                                VkBuffer bounds_buffer, VkDeviceSize bounds_size,
                                VkBuffer pipeline_bin_buffer, VkDeviceSize pipeline_bin_size,
                                uint32_t num_bins, uint32_t frame_index,
                                VkImageView hiz_view, VkSampler hiz_sampler,
                                uint32_t viewport_width, uint32_t viewport_height,
                                uint32_t hiz_mip_count, uint32_t is_second_pass);

    // Per-frame buffers
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> instance_buffers_;   // expand output
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> expand_counters_;    // atomic counter
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> visible_buffers_;    // cull output (binned)
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> indirect_buffers_;   // N dispatch cmds
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> culled_buffers_;     // Hi-Z rejected (first pass)
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> culled_counters_;    // atomic counter for culled
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> capacities_{};
};

} // namespace fjell
