#pragma once

#include "renderer/pass/render_pass.hpp"
#include "renderer/gpu/buffer.hpp"
#include "renderer/gpu/descriptor.hpp"
#include "renderer/renderer_constants.hpp"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>

namespace fjell {

struct GpuDrawAABB {
    float min_x, min_y, min_z, pad0;
    float max_x, max_y, max_z, pad1;
};

static_assert(sizeof(GpuDrawAABB) == 32, "GpuDrawAABB must be 32 bytes for std430");

class CullPass : public RenderPass {
public:
    [[nodiscard]] Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext&) override {}
    void pre_record(const FrameContext& ctx, GpuContext& gpu) override;

    // Legacy create — forwards to standard create
    [[nodiscard]] Result<> create(VkDevice device, VmaAllocator allocator,
                                  VkDescriptorSetLayout global_set_layout,
                                  VkPipelineCache pipeline_cache = VK_NULL_HANDLE);

    // Upload AABBs for this frame (called from IndirectBatch::build or vulkan_context)
    void upload_aabbs(const GpuDrawAABB* data, uint32_t count, uint32_t frame_index);

    // Dispatch the cull compute shader
    void dispatch(VkCommandBuffer cmd, VkDescriptorSet global_set,
                  VkBuffer mesh_indirect_buffer, uint32_t total_draws,
                  uint32_t frame_index);

    // Insert barrier: compute write → indirect/task read
    void insert_barrier(VkCommandBuffer cmd);

    [[nodiscard]] VkBuffer visibility_buffer(uint32_t frame_index) const {
        return visibility_buffers_[frame_index] ? visibility_buffers_[frame_index]->handle() : VK_NULL_HANDLE;
    }
    [[nodiscard]] VkDeviceSize visibility_buffer_size(uint32_t frame_index) const {
        return visibility_buffers_[frame_index] ? visibility_buffers_[frame_index]->size() : 0;
    }

    [[nodiscard]] bool enabled() const { return enabled_; }
    void set_enabled(bool e) { enabled_ = e; }

private:
    void ensure_capacity(uint32_t frame_index, uint32_t count);

    VkDevice device_{VK_NULL_HANDLE};
    VkPipelineCache vk_pipeline_cache_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};

    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};

    std::unique_ptr<DescriptorSetLayout> set_layout_;
    std::unique_ptr<DescriptorPool> pool_;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> descriptor_sets_{};

    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> aabb_buffers_;
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> visibility_buffers_;
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> capacities_{};

    bool enabled_{true};
};

} // namespace fjell
