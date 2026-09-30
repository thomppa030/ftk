#pragma once

#include "renderer/bindless.hpp"
#include "gpu/vulkan/frame_descriptor_cache.hpp"
#include "renderer/transient_image_pool.hpp"
#include "renderer/global_bindings.hpp"
#include "renderer/gpu/buffer.hpp"
#include "renderer/gpu/descriptor.hpp"
#include "renderer/gpu/gpu_core.hpp"
#include "gpu/swapchain.hpp"
#include "renderer/resources/mega_buffer.hpp"
#include "renderer/resources/meshlet_buffer.hpp"
#include "renderer/resources/meshlet_descriptor.hpp"
#include "renderer/pass/render_pass.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/render_stats.hpp"
#include "renderer/renderer_constants.hpp"
#include "renderer/gpu/thread_command_pools.hpp"
#include <vulkan/vulkan.h>

// Passes that still open zones with FJELL_GPU_ZONE reach Tracy's Vulkan
// macros through here.
#ifdef FJELL_ENABLE_TRACY
#include <tracy/TracyVulkan.hpp>
#endif

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>

namespace fjell {

class Window;

/// GPU device, swapchain, command pools, descriptors, sync objects,
/// shared buffers, and pipelines. Pure infrastructure — no scene or
/// pass knowledge.
class GpuContext {
public:
    explicit GpuContext(Window& window);

    /// Create the PipelineCache. Must be called after any external
    /// descriptor set layouts (e.g. DrawDataDescriptor) are created.
    void init_pipelines();
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
    /// What the main window shows.
    [[nodiscard]] gpu::Swapchain& swapchain() { return *swapchain_; }
    [[nodiscard]] const gpu::Swapchain& swapchain() const { return *swapchain_; }

    // ── Command infrastructure ──────────────────────────────────────────
    [[nodiscard]] VkCommandPool command_pool() const { return core_.command_pool(); }
    [[nodiscard]] ThreadCommandPools& secondary_pools() { return secondary_pools_; }
    void ensure_secondary_pools(uint32_t thread_count);

    [[nodiscard]] bool async_compute_supported() const {
        return core_.device().async_compute_supported();
    }

    // ── Deferred destruction ────────────────────────────────────────────

    // ── Batched GPU uploads ─────────────────────────────────────────────
    [[nodiscard]] UploadContext& upload_context() { return core_.upload_context(); }

    // ── Global descriptors ──────────────────────────────────────────────
    [[nodiscard]] GlobalBindings& global_bindings() { return global_bindings_; }
    [[nodiscard]] const GlobalBindings& global_bindings() const { return global_bindings_; }
    /// The global set's layout, for pipelines still made natively.
    [[nodiscard]] VkDescriptorSetLayout global_set_layout() const;
    /// The frame slot's GlobalUbo behind global binding "global_ubo", written
    /// through `mapped()`.
    [[nodiscard]] gpu::Buffer uniform_buffer(uint32_t frame_index) const {
        return uniform_buffers_[frame_index];
    }
    /// The frame slot's EnvironmentUbo behind global binding "environment_ubo".
    [[nodiscard]] gpu::Buffer environment_buffer(uint32_t frame_index) const {
        return environment_buffers_[frame_index];
    }

    /// Finalize global descriptor layout, pool, and initial writes.
    /// Must be called after any additional global_bindings().declare() calls
    /// and before init_pipelines().
    void finalize_descriptors();

    // ── Bindless ────────────────────────────────────────────────────────
    [[nodiscard]] BindlessTextures& bindless_textures() { return bindless_textures_; }

    // ── Per-frame descriptor set cache ─────────────────────────────────
    /// The GPU interface device's one-frame descriptor sets.
    [[nodiscard]] FrameDescriptorCache& descriptor_cache();
    [[nodiscard]] const FrameDescriptorCache& descriptor_cache() const;

    // ── Transient image pool (FrameGraph aliasing) ─────────────────────
    [[nodiscard]] TransientImagePool& transient_pool() { return transient_pool_; }

    // ── Shared GPU buffers ──────────────────────────────────────────────
    [[nodiscard]] MegaBuffer* mega_buffer() { return mega_buffer_.get(); }
    [[nodiscard]] MeshletBuffer* meshlet_buffer() { return meshlet_buffer_.get(); }
    [[nodiscard]] MeshletDescriptor& meshlet_descriptor() { return meshlet_descriptor_; }

    // ── Pipelines ───────────────────────────────────────────────────────
    [[nodiscard]] PipelineCache* pipeline_cache() { return pipeline_cache_.get(); }
    [[nodiscard]] VkPipelineCache vk_pipeline_cache() const { return vk_pipeline_cache_; }

    // ── Light data (global descriptor bindings) ─────────────────────────
    [[nodiscard]] gpu::Buffer light_ssbo(uint32_t frame_index) const { return light_ssbo_[frame_index]; }
    [[nodiscard]] gpu::Buffer tile_light_ssbo(uint32_t frame_index) const { return tile_light_ssbo_[frame_index]; }
    [[nodiscard]] gpu::Buffer fog_volume_ssbo(uint32_t frame_index) const { return fog_volume_ssbo_[frame_index]; }
    /// Nearest, clamped: for depth, which means nothing between two texels.
    [[nodiscard]] gpu::Sampler depth_sampler() const { return depth_sampler_; }
    /// A 1x1 black image for a 2D binding with nothing to hold, read through
    /// `gi_fallback_sampler()`.
    [[nodiscard]] gpu::Texture gi_fallback() const { return gi_fallback_; }
    /// A 1x1x1 volume, readable, for a 3D binding with nothing to hold.
    [[nodiscard]] gpu::Texture volume_fallback() const { return volume_fallback_; }
    [[nodiscard]] gpu::Sampler gi_fallback_sampler() const { return gi_fallback_sampler_; }

    // ── MSAA / mesh shaders ─────────────────────────────────────────────
    [[nodiscard]] VkSampleCountFlagBits msaa_samples() const { return msaa_samples_; }
    [[nodiscard]] bool ray_tracing_supported() const { return core_.device().ray_tracing_supported(); }

    // ── Stats ───────────────────────────────────────────────────────────
    [[nodiscard]] RenderStats& render_stats() { return *render_stats_; }

    // ── PassResources helper ────────────────────────────────────────────
    [[nodiscard]] PassResources pass_resources() const;

    /// The engine's shared set layouts as the GPU interface knows them.
    struct SharedLayouts {
        gpu::SharedLayout globals{};
        gpu::SharedLayout bindless{};
        gpu::SharedLayout draw_data{};
        gpu::SharedLayout meshlets{};
    };
    [[nodiscard]] const SharedLayouts& shared_layouts() const { return shared_layouts_; }


    /// A meshlet set as the GPU interface binds it, the same way.
    [[nodiscard]] gpu::BindGroup meshlets_group(VkDescriptorSet set);

    /// Takes the draw data layout, which the scene gatherer makes after this
    /// context, for the pipelines that name it.
    void share_draw_data_layout(gpu::SharedLayout layout);

    void wait_idle() { core_.wait_idle(); }

private:
    void create_descriptor_resources();

    GpuCore core_;
    std::unique_ptr<gpu::Swapchain> swapchain_;

    ThreadCommandPools secondary_pools_;
    bool secondary_pools_initialized_{false};

    // Descriptors
    GlobalBindings global_bindings_;
    std::array<gpu::Owned<gpu::Buffer>, MAX_FRAMES_IN_FLIGHT> uniform_buffers_;
    std::array<gpu::Owned<gpu::Buffer>, MAX_FRAMES_IN_FLIGHT> environment_buffers_;

    BindlessTextures bindless_textures_;
    TransientImagePool transient_pool_;

    // Pipelines
    VkPipelineCache vk_pipeline_cache_{VK_NULL_HANDLE};
    std::unique_ptr<PipelineCache> pipeline_cache_;

    // Shared GPU buffers
    std::unique_ptr<MegaBuffer> mega_buffer_;
    std::unique_ptr<MeshletBuffer> meshlet_buffer_;
    MeshletDescriptor meshlet_descriptor_;
    SharedLayouts shared_layouts_;
    /// Sets made before the interface, handed to the device as groups of a
    /// shared layout the first time a pass binds them.
    using AdoptedGroups = std::unordered_map<VkDescriptorSet, gpu::Owned<gpu::BindGroup>>;
    gpu::BindGroup adopted_group(AdoptedGroups& groups, gpu::SharedLayout layout, VkDescriptorSet set);
    AdoptedGroups meshlets_groups_;

    // Light data
    std::array<gpu::Owned<gpu::Buffer>, MAX_FRAMES_IN_FLIGHT> light_ssbo_;
    std::array<gpu::Owned<gpu::Buffer>, MAX_FRAMES_IN_FLIGHT> tile_light_ssbo_;
    std::array<gpu::Owned<gpu::Buffer>, MAX_FRAMES_IN_FLIGHT> fog_volume_ssbo_;
    gpu::Sampler depth_sampler_{};

    // Somewhere harmless for a binding whose writer is not in the graph being
    // drawn: a binding is only written by the pass that owns it, and a graph
    // need not contain that pass, while a shader sampling an unwritten
    // descriptor is undefined. Black for GI indirect when DDGI is off, and a
    // single empty texel for the 3D samplers.
    gpu::Owned<gpu::Texture> gi_fallback_;
    gpu::Owned<gpu::Texture> volume_fallback_;
    gpu::Sampler gi_fallback_sampler_{};

    VkSampleCountFlagBits msaa_samples_{VK_SAMPLE_COUNT_1_BIT};
    std::unique_ptr<RenderStats> render_stats_;

};

} // namespace fjell
