#pragma once

#include "renderer/bindless.hpp"
#include "renderer/gpu/buffer.hpp"
#include "renderer/gpu/descriptor.hpp"
#include "renderer/gpu/gpu_core.hpp"
#include "renderer/resources/mega_buffer.hpp"
#include "renderer/resources/meshlet_buffer.hpp"
#include "renderer/resources/meshlet_descriptor.hpp"
#include "renderer/pass/render_pass.hpp"
#include "renderer/gpu/pipeline.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/render_stats.hpp"
#include "renderer/renderer_constants.hpp"
#include "renderer/gpu/thread_command_pools.hpp"
#include <vulkan/vulkan.h>

#ifdef FJELL_ENABLE_TRACY
#include <tracy/TracyVulkan.hpp>
#endif

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace fjell {

class Window;

/// Insert a Vulkan image layout transition barrier (synchronization2).
void transition_image(VkCommandBuffer cmd, VkImage image,
                      VkImageLayout old_layout, VkImageLayout new_layout,
                      VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                      VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access,
                      VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);

/// GPU device, swapchain, command pools, descriptors, sync objects,
/// shared buffers, and pipelines. Pure infrastructure — no scene or
/// pass knowledge.
class GpuContext {
public:
    explicit GpuContext(Window& window);

    /// Create Pipeline and PipelineCache. Must be called after any external
    /// descriptor set layouts (e.g. DrawDataDescriptor) are created.
    void init_pipelines(VkDescriptorSetLayout draw_data_layout);
    ~GpuContext();

    GpuContext(const GpuContext&) = delete;
    GpuContext& operator=(const GpuContext&) = delete;
    GpuContext(GpuContext&&) = delete;
    GpuContext& operator=(GpuContext&&) = delete;

    // ── Core (forwarded from GpuCore) ──────────────────────────────────
    [[nodiscard]] GpuCore& core() { return core_; }
    [[nodiscard]] const GpuCore& core() const { return core_; }

    [[nodiscard]] Window& window() { return core_.window(); }
    [[nodiscard]] VkDevice device() const { return core_.vk_device(); }
    [[nodiscard]] VkPhysicalDevice physical_device() const { return core_.physical_device(); }
    [[nodiscard]] VmaAllocator allocator() const { return core_.allocator(); }
    [[nodiscard]] VkInstance instance() const { return core_.instance(); }
    [[nodiscard]] VkQueue graphics_queue() const { return core_.graphics_queue(); }
    [[nodiscard]] uint32_t graphics_family() const { return core_.graphics_family(); }
    [[nodiscard]] const std::string& gpu_name() const { return core_.device().gpu_name(); }
    [[nodiscard]] Device& device_ref() { return core_.device(); }

    // ── Swapchain ───────────────────────────────────────────────────────
    [[nodiscard]] Swapchain& swapchain() { return core_.swapchain(); }
    [[nodiscard]] VkFormat swapchain_format() const { return core_.swapchain_format(); }
    [[nodiscard]] VkExtent2D swapchain_extent() const { return core_.swapchain_extent(); }
    [[nodiscard]] uint32_t image_count() const { return core_.image_count(); }

    // ── Command infrastructure ──────────────────────────────────────────
    [[nodiscard]] VkCommandPool command_pool() const { return core_.command_pool(); }
    [[nodiscard]] ThreadCommandPools& secondary_pools() { return secondary_pools_; }
    void ensure_secondary_pools(uint32_t thread_count);

    [[nodiscard]] VkCommandBuffer command_buffer(uint32_t frame_index) const {
        return core_.command_buffer(frame_index);
    }

    // ── Frame sync ──────────────────────────────────────────────────────
    [[nodiscard]] VkSemaphore image_available_semaphore(uint32_t frame_index) const {
        return core_.image_available_semaphore(frame_index);
    }
    [[nodiscard]] VkFence in_flight_fence(uint32_t frame_index) const {
        return core_.in_flight_fence(frame_index);
    }

    // ── Global descriptors ──────────────────────────────────────────────
    [[nodiscard]] VkDescriptorSetLayout global_set_layout() const {
        return global_set_layout_->handle();
    }
    [[nodiscard]] VkDescriptorSet global_descriptor_set(uint32_t frame_index) const {
        return global_descriptor_sets_[frame_index];
    }
    [[nodiscard]] Buffer& uniform_buffer(uint32_t frame_index) {
        return *uniform_buffers_[frame_index];
    }

    // ── Bindless ────────────────────────────────────────────────────────
    [[nodiscard]] BindlessTextures& bindless_textures() { return bindless_textures_; }

    // ── Shared GPU buffers ──────────────────────────────────────────────
    [[nodiscard]] MegaBuffer* mega_buffer() { return mega_buffer_.get(); }
    [[nodiscard]] MeshletBuffer* meshlet_buffer() { return meshlet_buffer_.get(); }
    [[nodiscard]] MeshletDescriptor& meshlet_descriptor() { return meshlet_descriptor_; }

    // ── Pipelines ───────────────────────────────────────────────────────
    [[nodiscard]] Pipeline* pipeline() { return pipeline_.get(); }
    [[nodiscard]] PipelineCache* pipeline_cache() { return pipeline_cache_.get(); }
    [[nodiscard]] VkPipelineCache vk_pipeline_cache() const { return vk_pipeline_cache_; }

    // ── Light data (global descriptor bindings) ─────────────────────────
    [[nodiscard]] Buffer& light_ssbo(uint32_t frame_index) { return *light_ssbo_[frame_index]; }
    [[nodiscard]] Buffer& tile_light_ssbo(uint32_t frame_index) { return *tile_light_ssbo_[frame_index]; }
    [[nodiscard]] VkSampler depth_sampler() const { return depth_sampler_; }
    [[nodiscard]] VkImageView gi_fallback_view() const { return gi_fallback_view_; }
    [[nodiscard]] VkSampler gi_fallback_sampler() const { return gi_fallback_sampler_; }

    // ── MSAA / mesh shaders ─────────────────────────────────────────────
    [[nodiscard]] VkSampleCountFlagBits msaa_samples() const { return msaa_samples_; }
    [[nodiscard]] bool mesh_shaders_available() const { return core_.device().mesh_shader_supported(); }
    [[nodiscard]] bool use_mesh_shaders() const { return use_mesh_shaders_; }
    void set_use_mesh_shaders(bool enable) {
        use_mesh_shaders_ = enable && core_.device().mesh_shader_supported();
    }

    // ── Stats ───────────────────────────────────────────────────────────
    [[nodiscard]] RenderStats& render_stats() { return *render_stats_; }

    // ── PassResources helper ────────────────────────────────────────────
    [[nodiscard]] PassResources pass_resources() const;

    void wait_idle() { core_.wait_idle(); }

#ifdef FJELL_ENABLE_TRACY
    [[nodiscard]] tracy::VkCtx* tracy_ctx() { return tracy_vk_ctx_; }
#endif

private:
    void create_descriptor_resources();

    GpuCore core_;

    ThreadCommandPools secondary_pools_;
    bool secondary_pools_initialized_{false};

    // Descriptors
    std::unique_ptr<DescriptorSetLayout> global_set_layout_;
    std::unique_ptr<DescriptorPool> global_pool_;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> global_descriptor_sets_{};
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> uniform_buffers_;

    BindlessTextures bindless_textures_;

    // Pipelines
    VkPipelineCache vk_pipeline_cache_{VK_NULL_HANDLE};
    std::unique_ptr<Pipeline> pipeline_;
    std::unique_ptr<PipelineCache> pipeline_cache_;

    // Shared GPU buffers
    std::unique_ptr<MegaBuffer> mega_buffer_;
    std::unique_ptr<MeshletBuffer> meshlet_buffer_;
    MeshletDescriptor meshlet_descriptor_;

    // Light data
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> light_ssbo_;
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> tile_light_ssbo_;
    VkSampler depth_sampler_{VK_NULL_HANDLE};

    // 1x1 black fallback for GI indirect (binding 10 when DDGI disabled)
    VkImage gi_fallback_image_{VK_NULL_HANDLE};
    VmaAllocation gi_fallback_alloc_{VK_NULL_HANDLE};
    VkImageView gi_fallback_view_{VK_NULL_HANDLE};
    VkSampler gi_fallback_sampler_{VK_NULL_HANDLE};

    VkSampleCountFlagBits msaa_samples_{VK_SAMPLE_COUNT_1_BIT};
    bool use_mesh_shaders_{false};
    std::unique_ptr<RenderStats> render_stats_;

#ifdef FJELL_ENABLE_TRACY
    tracy::VkCtx* tracy_vk_ctx_{nullptr};
#endif
};

} // namespace fjell
