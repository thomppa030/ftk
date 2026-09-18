#pragma once

#include "renderer/pass/render_pass.hpp"

#include <glm/glm.hpp>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>

#include "renderer/renderer_constants.hpp"

namespace fjell {

// ── Froxel-based volumetric fog pipeline ────────────────────────────────────
//
// Fog runs four compute dispatches authored as four independent DAG nodes:
//
//   fog_inject     — write density + light scattering into scatter[cur].
//   fog_temporal   — blend scatter[cur] with scatter[prev] (the last
//                    frame's result). Reads scatter[prev], reads+writes
//                    scatter[cur].
//   fog_integrate  — front-to-back ray march scatter[cur] into the
//                    single-frame transient fog_integrated.
//   fog_composite  — apply fog_integrated over the scene color.
//
// With each dispatch as its own RenderPass, the FrameGraph emits one
// merged barrier per image per node and the single-frame fog_integrated
// volume's lifetime collapses to [integrate..composite] (alias-eligible
// with other single-frame transients of matching shape).
//
// Layering:
//   • FogCore          — abstract base. Owns sampler, four descriptor
//                        layouts, four pipelines, scene parameters, the
//                        per-viewport Images struct (scatter ping-pong),
//                        and prepare_frame() to source scene state. Itself
//                        a RenderPass, registered as "fog_inject" through
//                        FogInjectPass.
//   • FogInjectPass    — concrete FogCore. Advances the ping-pong counter
//                        at declare time so followers see a consistent
//                        (cur, prev) pair for the whole frame. Runs the
//                        inject dispatch.
//   • FogTemporalPass,
//     FogIntegratePass,
//     FogCompositePass — followers. Hold FogCore* set in post_create()
//                        via passes.get<FogCore>("fog_inject"). Each
//                        records exactly one dispatch.
//
// Resource flow:
//   • scatter[0] / scatter[1] are persistent per-viewport Images (cross-
//     frame temporal reuse). RenderPipeline::execute injects them as
//     "fog_scatter_cur" / "fog_scatter_prev" into DeclareContext.imports
//     each frame based on the active viewport's ping-pong state, same
//     pattern Hi-Z and GTAO use for their per-viewport images.
//   • fog_integrated is a graph transient: FogIntegratePass create()s it,
//     FogCompositePass create()s it again for the dedup-by-name read.
//
// Barrier contract: nodes declare only the accesses they perform. The
// graph emits one merged barrier per image per node. No inline barriers
// — scatter layout state carries across frames through the graph's
// per-slice tracker, and fog_integrated is fully owned by the graph.

class FogCore : public RenderPass {
public:
    static constexpr uint32_t TILE_SIZE = 8;
    static constexpr uint32_t DEPTH_SLICES = 128;
    static constexpr uint32_t SCREEN_DIVISOR = 16;
    static constexpr VkFormat VOLUME_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;

    struct VolumeImage {
        VkImage image{VK_NULL_HANDLE};
        VmaAllocation memory{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};
    };

    struct Images {
        VolumeImage scatter_volumes[2];

        VkExtent3D volume_extent{};
        uint32_t frame_counter{0};
        uint32_t ping_pong{0};  // index of the "current" volume this frame

        void create(VkDevice device, VmaAllocator allocator,
                     uint32_t vol_w, uint32_t vol_h);
        void destroy(VkDevice device, VmaAllocator allocator);
        void resize(VkDevice device, VmaAllocator allocator,
                     uint32_t screen_w, uint32_t screen_h);

        void invalidate_imgui_texture();

        [[nodiscard]] uint32_t cur_index() const { return ping_pong; }
        [[nodiscard]] uint32_t prev_index() const { return 1 - ping_pong; }
    };

    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void prepare_frame(const FrameContext& ctx, GpuContext& gpu) override;

    void resize(uint32_t width, uint32_t height);

    [[nodiscard]] bool enabled() const { return enabled_; }

    // Getters for follower passes.
    [[nodiscard]] VkSampler fog_sampler() const { return linear_sampler_; }
    [[nodiscard]] VkDescriptorSetLayout inject_set_layout() const { return inject_set_layout_; }
    [[nodiscard]] VkDescriptorSetLayout temporal_set_layout() const { return temporal_set_layout_; }
    [[nodiscard]] VkDescriptorSetLayout integrate_set_layout() const { return integrate_set_layout_; }
    [[nodiscard]] VkDescriptorSetLayout composite_set_layout() const { return composite_set_layout_; }
    [[nodiscard]] VkPipelineLayout inject_pipeline_layout() const { return inject_pipeline_layout_; }
    [[nodiscard]] VkPipelineLayout temporal_pipeline_layout() const { return temporal_pipeline_layout_; }
    [[nodiscard]] VkPipelineLayout integrate_pipeline_layout() const { return integrate_pipeline_layout_; }
    [[nodiscard]] VkPipelineLayout composite_pipeline_layout() const { return composite_pipeline_layout_; }
    [[nodiscard]] VkPipeline inject_pipeline() const { return inject_pipeline_; }
    [[nodiscard]] VkPipeline temporal_pipeline() const { return temporal_pipeline_; }
    [[nodiscard]] VkPipeline integrate_pipeline() const { return integrate_pipeline_; }
    [[nodiscard]] VkPipeline composite_pipeline() const { return composite_pipeline_; }
    [[nodiscard]] Images& images() { return default_images_; }

    // Scene-driven fog parameters + push-constant helper read by each node.
    struct PushConstants {
        glm::uvec4 volume_dims;   // xyz = froxel grid, w = frame_index
        float near_plane;
        float far_plane;
        float density;
        float height;
        float height_falloff;
        float scattering;
        float anisotropy;
        float temporal_blend;
        uint32_t viewport_width;
        uint32_t viewport_height;
        float fog_color_r;
        float fog_color_g;
        float fog_color_b;
        float jitter_x;
        float jitter_y;
        float jitter_z;
        // How many entries of fog_volume_ssbo the inject pass should read.
        // The other three fog shaders share this block and ignore it.
        uint32_t volume_count;
        // How far the height fog answers things moving through it. Read by
        // the inject pass; shared with the other three the same way.
        float motion_response;
        // Paints the stirred air so the density field can be seen. 0 is off.
        float density_field_debug;
    };
    [[nodiscard]] PushConstants make_push(const Images& imgs,
                                           uint32_t width, uint32_t height) const;

private:
    static VolumeImage create_volume_image(VkDevice device, VmaAllocator allocator,
                                            uint32_t w, uint32_t h, uint32_t d);
    static void destroy_volume_image(VkDevice device, VmaAllocator allocator, VolumeImage& img);

    // Scene-driven fog parameters (set by prepare_frame)
    bool enabled_{false};
    float density_{0.003f};
    float height_{0.0f};
    float height_falloff_{0.5f};
    float scattering_{0.5f};
    float anisotropy_{0.7f};
    float max_distance_{130.0f};
    float temporal_blend_{0.9f};
    float motion_response_{0.0f};
    float density_field_debug_{0.0f};
    glm::vec3 fog_color_{0.8f, 0.85f, 0.9f};
    uint32_t volume_count_{0};

    VkDevice device_{VK_NULL_HANDLE};
    VkPipelineCache vk_pipeline_cache_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};

    VkSampler linear_sampler_{VK_NULL_HANDLE};

    VkDescriptorSetLayout inject_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout inject_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline inject_pipeline_{VK_NULL_HANDLE};

    VkDescriptorSetLayout temporal_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout temporal_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline temporal_pipeline_{VK_NULL_HANDLE};

    VkDescriptorSetLayout integrate_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout integrate_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline integrate_pipeline_{VK_NULL_HANDLE};

    VkDescriptorSetLayout composite_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout composite_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline composite_pipeline_{VK_NULL_HANDLE};

    Images default_images_;
};

// Concrete inject node — IS the FogCore. Records inject dispatch, also
// advances the ping-pong counter at declare time so all four nodes see
// the same (cur, prev) pair for the whole frame.
class FogInjectPass : public FogCore {
public:
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void record(const FrameContext& ctx) override;
};

class FogTemporalPass : public RenderPass {
public:
    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void post_create(GpuContext& gpu, PassRegistry& passes) override;

private:
    FogCore* core_{nullptr};
};

class FogIntegratePass : public RenderPass {
public:
    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void post_create(GpuContext& gpu, PassRegistry& passes) override;

private:
    FogCore* core_{nullptr};
};

class FogCompositePass : public RenderPass {
public:
    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void post_create(GpuContext& gpu, PassRegistry& passes) override;

private:
    FogCore* core_{nullptr};
};

// Deprecated type alias: PostProcessResources still refers to
// "VolumetricFogPass" in several callers. Point them at the new
// abstract FogCore so the Images + layouts stay reachable through a
// single type everyone agrees on.
using VolumetricFogPass = FogCore;

} // namespace fjell
