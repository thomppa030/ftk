#pragma once

#include "gpu/owned.hpp"
#include "gpu/texture.hpp"
#include "renderer/pass/render_pass.hpp"
#include "renderer/frustum.hpp"
#include "renderer/gpu/buffer.hpp"
#include "renderer/renderer_constants.hpp"
#include "renderer/render_settings.hpp"

#include <glm/glm.hpp>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <memory>

namespace fjell {

class AccelStructureManager;

/// Reprojection matrices for the reflection denoise.
///
/// Three mat4s do not fit the guaranteed 128-byte push constant range, so
/// they travel in a UBO (the same reason `GpuSsGiConstants` exists).
struct GpuReflectionDenoiseConstants {
    glm::mat4 projection{1.0F};
    glm::mat4 inv_view_proj{1.0F};
    glm::mat4 prev_view_proj{1.0F};
    glm::mat4 prev_inv_view_proj{1.0F};
    glm::vec3 camera_position{0.0F};
    float pad0{0.0F};
};

// Ray-traced reflections. Slots into the same composite as SSR by writing
// rt_reflection_output (RGBA16F, viewport-sized). The composite picks
// between ssr_output and rt_reflection_output based on the active mode
// on SsrCore.
//
// Skinned meshes are not in the BLAS in V1 — they don't appear in
// reflections. Static meshes and particles do.
//
// Hardware-gated: the RT pipeline is only built when
// device.ray_tracing_supported(). When unsupported the pass still
// instantiates so .fjgraph parses, but record() short-circuits.
class RtReflectionPass : public RenderPass {
public:
    using Mode = RenderSettings::Ssr::Mode;

    /// Per-viewport image and descriptor state. Owned by
    /// PostProcessResources, one instance per viewport. Pass singleton
    /// owns shared state (samplers, set layout, RT pipeline, SBT).
    struct Images {
        // Denoised result. This is what the composite reads.
        VkImage out_image{VK_NULL_HANDLE};
        /// The denoised and raw results as the GPU interface's textures.
        gpu::Owned<gpu::Texture> out_texture;
        gpu::Owned<gpu::Texture> raw_texture;
        VmaAllocation out_alloc{VK_NULL_HANDLE};
        VkImageView out_view{VK_NULL_HANDLE};
        VkExtent2D out_extent{};
        VkImageLayout out_layout{VK_IMAGE_LAYOUT_UNDEFINED};

        // Raw trace output, before accumulation.
        VkImage raw_image{VK_NULL_HANDLE};
        VmaAllocation raw_alloc{VK_NULL_HANDLE};
        VkImageView raw_view{VK_NULL_HANDLE};

        // Previous frame's denoised result, blended into this one.
        VkImage history_image{VK_NULL_HANDLE};
        VmaAllocation history_alloc{VK_NULL_HANDLE};
        VkImageView history_view{VK_NULL_HANDLE};

        // Depth as the denoise saw it, for the disocclusion test. Two images,
        // ping-ponged: the dispatch samples last frame's at an arbitrary
        // reprojected texel while writing this frame's, so one image would race
        // itself. R32F colour, written by the shader — see the note in
        // rt_reflection_denoise.comp on why this is not a copy of the depth
        // buffer.
        std::array<VkImage, 2> prev_depth_image{VK_NULL_HANDLE, VK_NULL_HANDLE};
        std::array<VmaAllocation, 2> prev_depth_alloc{VK_NULL_HANDLE, VK_NULL_HANDLE};
        std::array<VkImageView, 2> prev_depth_view{VK_NULL_HANDLE, VK_NULL_HANDLE};
        uint32_t prev_depth_slot{0};
        VkExtent2D depth_extent{};
        bool prev_depth_valid{false};
        bool history_valid{false};

        // Per-viewport camera history. Each viewport reprojects through its own
        // previous camera — a shared copy would let the last viewport to render
        // decide what "previous frame" means for all the others (see
        // ViewHistory's own note).
        glm::mat4 inv_view_proj{1.0F};
        glm::mat4 prev_view_proj{1.0F};
        glm::mat4 prev_inv_view_proj{1.0F};

        // History images are genuinely sequential, not per-frame-in-flight:
        // one frame's result is the next frame's input. Only the UBO and
        // descriptor sets are per-frame.
        std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> denoise_ubo;
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> denoise_set{};
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> spatial_set{};

        // One uniform buffer and one trace set per frame in flight. The UBO
        // is host-written every frame and the set binds the slot's TLAS and
        // per-instance table through update-after-bind, so a frame still
        // pending on the GPU must not share either with the frame being
        // recorded.
        std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> reflection_ubo;
        VkDescriptorPool desc_pool{VK_NULL_HANDLE};
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> trace_set{};
        bool first_use{true};

        void create(gpu::Device& gpu, VkDevice device, VmaAllocator allocator,
                     VkDescriptorSetLayout set_layout,
                     VkDescriptorSetLayout denoise_layout,
                     VkDescriptorSetLayout spatial_layout,
                     VkFormat depth_format,
                     uint32_t width, uint32_t height);
        void destroy(VkDevice device, VmaAllocator allocator);
        void resize(gpu::Device& gpu, VkDevice device, VmaAllocator allocator,
                     VkDescriptorSetLayout set_layout,
                     VkDescriptorSetLayout denoise_layout,
                     VkDescriptorSetLayout spatial_layout,
                     VkFormat depth_format,
                     uint32_t width, uint32_t height);
    };

    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;

    void set_accel_manager(AccelStructureManager* mgr) { accel_manager_ = mgr; }
    void set_mode(Mode m) { mode_ = m; }
    [[nodiscard]] Mode mode() const { return mode_; }
    [[nodiscard]] bool ray_traced_active() const {
        return mode_ == Mode::RayTraced && rt_pipeline_ != VK_NULL_HANDLE;
    }

    void set_sun_light(glm::vec3 direction, glm::vec3 color, float intensity) {
        sun_direction_ = direction;
        sun_color_ = color;
        sun_intensity_ = intensity;
    }

    void set_intensity(float i) { intensity_ = i; }
    [[nodiscard]] float intensity() const { return intensity_; }

    /// How far a reflection ray travels, in metres. The same value bounds the
    /// trace and is declared to the acceleration structure as this pass's
    /// reach, so the TLAS is built to cover what the rays can actually visit.
    /// Read them from here rather than restating the distance — a reach
    /// shorter than the trace is invisible until something reflective is far
    /// enough from the probe grid to notice, which is the bug this replaced.
    void set_max_trace_dist(float d) { max_trace_dist_ = d; }
    [[nodiscard]] float max_trace_dist() const { return max_trace_dist_; }

    /// The coarsest cell in the traced geometry, in metres — terrain's ray
    /// mesh spacing, which is the only geometry traced coarser than it is
    /// shaded. Rays start further out of the surface on slopes by this much,
    /// or they begin inside the mesh they are about to trace. Zero when no
    /// terrain is loaded, which puts rays back on the flat-ground minimum.
    void set_trace_cell_size(float m) { trace_cell_size_ = m; }
    [[nodiscard]] float trace_cell_size() const { return trace_cell_size_; }

    /// The box this pass's rays can reach from `camera`, for
    /// AccelStructureManager::register_trace_reach. Rays leave the camera in
    /// every direction, so it is a cube around it rather than a frustum:
    /// a reflection bounces backwards as readily as forwards.
    [[nodiscard]] AABB trace_reach_bounds(const glm::vec3& camera) const {
        return {camera - glm::vec3(max_trace_dist_), camera + glm::vec3(max_trace_dist_)};
    }

    [[nodiscard]] VkDescriptorSetLayout trace_set_layout() const { return trace_set_layout_; }
    [[nodiscard]] VkDescriptorSetLayout denoise_set_layout() const { return denoise_set_layout_; }
    [[nodiscard]] VkDescriptorSetLayout spatial_set_layout() const { return spatial_set_layout_; }
    [[nodiscard]] VkFormat depth_format() const { return depth_format_; }

private:
    void record_into(const FrameContext& ctx, Images& imgs);
    void record_denoise(const FrameContext& ctx, Images& imgs);
    void record_spatial(const FrameContext& ctx, Images& imgs);

    Device* device_{nullptr};
    VkDevice vk_device_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};
    AccelStructureManager* accel_manager_{nullptr};

    Mode mode_{Mode::ScreenSpace};
    float intensity_{1.0F};
    float max_trace_dist_{50.0F};
    float trace_cell_size_{0.0F};
    uint32_t frame_counter_{0};

    // Temporal blend: how much of the accumulated history survives each frame.
    // 0.9 converges in roughly ten frames. Tune against the rain scene rather
    // than a static one — the ripples animate the reflected image on their own,
    // and a value picked while nothing moves will visibly smear them.
    float hysteresis_{0.9F};

    glm::vec3 sun_direction_{0.0F, -1.0F, 0.0F};
    glm::vec3 sun_color_{1.0F};
    float sun_intensity_{1.5F};

    // Shared (pass-singleton) state. NULL when device lacks RT support.
    VkSampler linear_sampler_{VK_NULL_HANDLE};
    VkSampler nearest_sampler_{VK_NULL_HANDLE};
    VkDescriptorSetLayout trace_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout rt_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline rt_pipeline_{VK_NULL_HANDLE};

    // Temporal denoise (compute). Built alongside the RT pipeline.
    VkDescriptorSetLayout denoise_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout denoise_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline denoise_pipeline_{VK_NULL_HANDLE};
    VkFormat depth_format_{VK_FORMAT_UNDEFINED};

    // Spatial filter (compute), run on the accumulated result.
    VkDescriptorSetLayout spatial_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout spatial_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline spatial_pipeline_{VK_NULL_HANDLE};

    // Scales the roughness-driven kernel radius. 0 disables the pass, leaving
    // the composite reading the accumulated image directly.
    float spatial_strength_{1.0F};

    // Shader binding table (shared)
    std::unique_ptr<Buffer> sbt_buffer_;
    VkStridedDeviceAddressRegionKHR sbt_raygen_{};
    VkStridedDeviceAddressRegionKHR sbt_miss_{};
    VkStridedDeviceAddressRegionKHR sbt_hit_{};
    VkStridedDeviceAddressRegionKHR sbt_callable_{};
};

} // namespace fjell
