#pragma once

#include "renderer/bindless.hpp"
#include "ftk/framegraph/transient_image_pool.hpp"
#include "renderer/global_bindings.hpp"
#include "ftk/gpu/device.hpp"
#include "ftk/gpu/swapchain.hpp"
#include "renderer/resources/mega_buffer.hpp"
#include "renderer/resources/meshlet_buffer.hpp"
#include "renderer/resources/meshlet_descriptor.hpp"
#include "renderer/pass/render_pass.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/render_stats.hpp"
#include "renderer/renderer_constants.hpp"

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

    // ── Device ──────────────────────────────────────────────────────────
    /// The GPU, made for the window.
    [[nodiscard]] gpu::Device& device() { return *device_; }
    [[nodiscard]] const gpu::Device& device() const { return *device_; }

    [[nodiscard]] Window& window() { return window_; }
    [[nodiscard]] const std::string& gpu_name() const { return device_->name(); }

    // ── Swapchain ───────────────────────────────────────────────────────
    /// What the main window shows.
    [[nodiscard]] gpu::Swapchain& swapchain() { return *swapchain_; }
    [[nodiscard]] const gpu::Swapchain& swapchain() const { return *swapchain_; }

    // ── Command infrastructure ──────────────────────────────────────────

    [[nodiscard]] bool async_compute_supported() const {
        return device_->caps().async_compute;
    }

    // ── Global descriptors ──────────────────────────────────────────────
    [[nodiscard]] GlobalBindings& global_bindings() { return global_bindings_; }
    [[nodiscard]] const GlobalBindings& global_bindings() const { return global_bindings_; }
    /// Points the bindings a view's run fills in `slot`'s group (its
    /// uniforms, environment, lights and fog volumes, and its depth, AO, GI,
    /// contact shadows and screen colour) at zeroed data and harmless images.
    /// A run writes its data into the frame's memory, which a slot hands out
    /// anew each time it comes round, so nothing may read what a view set
    /// there in the slot's last frame.
    void reset_view_bindings(uint32_t slot);

    /// Finalize global descriptor layout, pool, and initial writes.
    /// Must be called after any additional global_bindings().declare() calls
    /// and before init_pipelines().
    void finalize_descriptors();

    // ── Bindless ────────────────────────────────────────────────────────
    [[nodiscard]] BindlessTextures& bindless_textures() { return bindless_textures_; }

    // ── Transient image pool (FrameGraph aliasing) ─────────────────────
    [[nodiscard]] TransientImagePool& transient_pool() { return transient_pool_; }

    // ── Shared GPU buffers ──────────────────────────────────────────────
    [[nodiscard]] MegaBuffer* mega_buffer() { return mega_buffer_.get(); }
    [[nodiscard]] MeshletBuffer* meshlet_buffer() { return meshlet_buffer_.get(); }
    [[nodiscard]] MeshletDescriptor& meshlet_descriptor() { return meshlet_descriptor_; }

    // ── Pipelines ───────────────────────────────────────────────────────
    [[nodiscard]] PipelineCache* pipeline_cache() { return pipeline_cache_.get(); }

    // ── Light data (global descriptor bindings) ─────────────────────────
    [[nodiscard]] gpu::Buffer tile_light_ssbo(uint32_t frame_index) const { return tile_light_ssbo_[frame_index]; }
    /// Nearest, clamped: for depth, which means nothing between two texels.
    [[nodiscard]] gpu::Sampler depth_sampler() const { return depth_sampler_; }
    /// A 1x1 black image for a 2D binding with nothing to hold, read through
    /// `gi_fallback_sampler()`.
    [[nodiscard]] gpu::Texture gi_fallback() const { return gi_fallback_; }
    /// A 1x1x1 volume, readable, for a 3D binding with nothing to hold.
    [[nodiscard]] gpu::Texture volume_fallback() const { return volume_fallback_; }
    /// A 1x1 image reading one, nothing occluding: the AO and contact
    /// shadows of a view that computes neither. Read through
    /// `gi_fallback_sampler()`.
    [[nodiscard]] gpu::Texture unoccluded() const { return unoccluded_; }
    [[nodiscard]] gpu::Sampler gi_fallback_sampler() const { return gi_fallback_sampler_; }

    // ── MSAA / mesh shaders ─────────────────────────────────────────────
    [[nodiscard]] gpu::Samples msaa_samples() const { return msaa_samples_; }
    [[nodiscard]] bool ray_tracing_supported() const { return device_->caps().ray_queries; }

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



    /// Takes the draw data layout, which the scene gatherer makes after this
    /// context, for the pipelines that name it.
    void share_draw_data_layout(gpu::SharedLayout layout);

    void wait_idle() { device_->wait_idle(); }

private:
    void create_descriptor_resources();

    Window& window_;
    std::unique_ptr<gpu::Device> device_;
    std::unique_ptr<gpu::Swapchain> swapchain_;


    // Descriptors
    GlobalBindings global_bindings_;
    /// Zeroes, as large as the largest of a view's uniforms, lights and fog
    /// volumes: what those bindings read before a view has written its own.
    gpu::Owned<gpu::Buffer> view_data_fallback_;

    BindlessTextures bindless_textures_;
    TransientImagePool transient_pool_;

    // Pipelines
    std::unique_ptr<PipelineCache> pipeline_cache_;

    // Shared GPU buffers
    std::unique_ptr<MegaBuffer> mega_buffer_;
    std::unique_ptr<MeshletBuffer> meshlet_buffer_;
    MeshletDescriptor meshlet_descriptor_;
    SharedLayouts shared_layouts_;

    // Light data
    std::array<gpu::Owned<gpu::Buffer>, MAX_FRAMES_IN_FLIGHT> tile_light_ssbo_;
    gpu::Sampler depth_sampler_{};

    // Somewhere harmless for a binding whose writer is not in the graph being
    // drawn: a binding is only written by the pass that owns it, and a graph
    // need not contain that pass, while a shader sampling an unwritten
    // descriptor is undefined. Black for GI indirect when DDGI is off, and a
    // single empty texel for the 3D samplers.
    gpu::Owned<gpu::Texture> gi_fallback_;
    gpu::Owned<gpu::Texture> volume_fallback_;
    gpu::Owned<gpu::Texture> unoccluded_;
    gpu::Sampler gi_fallback_sampler_{};

    gpu::Samples msaa_samples_{gpu::Samples::x1};
    std::unique_ptr<RenderStats> render_stats_;

};

} // namespace fjell
