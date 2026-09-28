#pragma once

#include "renderer/pass/render_pass.hpp"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>

namespace fjell {

// ── Screen-space ambient occlusion pipeline ─────────────────────────────────
//
// AO runs three compute dispatches authored as three independent DAG nodes:
//
//   ao_trace    — trace occlusion, write half-res ao_image. Algorithm-specific
//                 (GTAO today; VBAO/HBAO can live alongside as sibling AoPass
//                 subclasses).
//   ao_blur_h   — horizontal separable blur, writes transient gtao_blur.
//   ao_blur_v   — vertical blur, writes back to ao_image.
//
// Each dispatch is its own RenderPass so the FrameGraph can schedule barriers
// per node (one merged barrier per image per node) and the blur scratch
// participates in alias bin-packing — its lifetime is only [blur_h, blur_v].
//
// Layering:
//   • AoPass        — abstract base. Owns shared state (ao_image Images,
//                     ao_sampler, blur descriptor set layout + pipeline, shared
//                     parameters). Itself a RenderPass registered as "ao_trace".
//                     Subclasses implement the trace dispatch.
//   • GtaoTracePass — concrete AoPass. Owns the GTAO trace pipeline +
//                     descriptor layout; implements the trace hooks.
//   • AoBlurHPass,
//     AoBlurVPass   — follower passes. Hold AoPass* set in post_create() via
//                     passes.get<AoPass>("ao_trace"). Record one blur dispatch
//                     each, reading the owner's shared state.
//
// Runtime algorithm switching: register exactly one AoPass subclass under
// "ao_trace". To support GTAO/VBAO toggles, register both and use
// RenderPipeline::set_pass_enabled to flip between them.
//
// Resource flow:
//   • ao_image is owned by AoPass::Images, exported via collect_exports() as
//     "gtao_ao" with persistent=true (global descriptor binding 5 "ao_texture"
//     plus ImGui thumbnail demand a stable VkImage across frames).
//   • gtao_blur is a graph transient declared by AoBlurHPass via create().
//     Lifetime [blur_h, blur_v], alias-eligible with other half-res R8
//     transients.
//
// Barrier contract: nodes declare reads/writes; the graph emits one merged
// barrier per image per node, and no node emits one of its own. The AO
// image rests where the global descriptor samples it, and the graph returns
// it there after each node that writes it.

class AoPass : public RenderPass {
public:
    struct Images {
        VkImage ao_image{VK_NULL_HANDLE};
        VmaAllocation ao_memory{VK_NULL_HANDLE};
        VkImageView ao_view{VK_NULL_HANDLE};

        VkExtent2D extent{};  // half-res dimensions
        VkDescriptorSet imgui_descriptor{VK_NULL_HANDLE};
        bool images_need_init{true};

        // `concurrent_families` + `concurrent_family_count` come from
        // `Device::concurrent_queue_families()`. When non-null the AO
        // image is created with `VK_SHARING_MODE_CONCURRENT` so the
        // compute queue can write it while graphics reads through the
        // global descriptor. Null on hardware without an async compute
        // queue family; the image falls back to exclusive sharing.
        void create(VkDevice device, VmaAllocator allocator,
                     const uint32_t* concurrent_families,
                     uint32_t concurrent_family_count,
                     uint32_t half_w, uint32_t half_h,
                     VkQueue graphics_queue = VK_NULL_HANDLE,
                     VkCommandPool graphics_pool = VK_NULL_HANDLE);
        void destroy(VkDevice device, VmaAllocator allocator);
        void resize(VkDevice device, VmaAllocator allocator,
                     const uint32_t* concurrent_families,
                     uint32_t concurrent_family_count,
                     uint32_t width, uint32_t height,
                     VkQueue graphics_queue = VK_NULL_HANDLE,
                     VkCommandPool graphics_pool = VK_NULL_HANDLE);

        [[nodiscard]] VkDescriptorSet imgui_texture(VkSampler sampler);
        void invalidate_imgui_texture();
    };

    Result<> create(Device& device, const PassResources& resources) final;
    void destroy() final;
    void record(const FrameContext& ctx) final;
    void declare(PassBuilder& builder, const DeclareContext& ctx) final;
    void collect_exports(DeclareContext& ctx) final;

    void resize(uint32_t width, uint32_t height);

    [[nodiscard]] VkImageView ao_view() const { return default_images_.ao_view; }
    [[nodiscard]] VkImage ao_image() const { return default_images_.ao_image; }
    [[nodiscard]] VkSampler ao_sampler() const { return ao_sampler_; }

    [[nodiscard]] VkDescriptorSet imgui_texture();
    void invalidate_imgui_texture();

    void set_radius(float r) { radius_ = r; }
    void set_intensity(float i) { intensity_ = i; }
    void set_thickness(float t) { thickness_ = t; }
    void set_enabled(bool e) { enabled_ = e; }
    [[nodiscard]] float radius() const { return radius_; }
    [[nodiscard]] float intensity() const { return intensity_; }
    [[nodiscard]] float thickness() const { return thickness_; }
    [[nodiscard]] bool enabled() const { return enabled_; }

    [[nodiscard]] VkDescriptorSetLayout blur_set_layout() const { return blur_set_layout_; }
    [[nodiscard]] VkPipelineLayout blur_pipeline_layout() const { return blur_layout_; }
    [[nodiscard]] VkPipeline blur_pipeline() const { return blur_pipeline_; }
    [[nodiscard]] Images& images() { return default_images_; }

    /// Concurrent queue family info forwarded to per-viewport `Images`
    /// created through `PostProcessResources`. Null on devices without a
    /// dedicated compute queue.
    [[nodiscard]] const uint32_t* concurrent_queue_families() const { return concurrent_families_; }
    [[nodiscard]] uint32_t concurrent_queue_family_count() const { return concurrent_family_count_; }

    /// Graphics queue + command pool for one-shot image transitions during
    /// PostProcessResources::Images::create. Set via post_create; null for
    /// headless/test builds where PostProcessResources never calls them.
    [[nodiscard]] VkQueue init_graphics_queue() const { return init_graphics_queue_; }
    [[nodiscard]] VkCommandPool init_graphics_pool() const { return init_graphics_pool_; }

protected:
    // Subclass hooks. AoPass::create() sets up shared state then calls
    // create_trace(); AoPass::declare/record delegate when enabled, and
    // handle the !enabled_ clear-to-white path directly.
    virtual Result<> create_trace(Device& device, const PassResources& resources) = 0;
    virtual void declare_trace(PassBuilder& builder, const DeclareContext& ctx) = 0;
    virtual void record_trace(const FrameContext& ctx, Images& images) = 0;
    virtual void destroy_trace() = 0;

    // Shared Vulkan context for subclasses.
    VkDevice device_{VK_NULL_HANDLE};
    VkPipelineCache vk_pipeline_cache_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};
    VkDescriptorSetLayout global_set_layout_{VK_NULL_HANDLE};
    // Cached concurrent-sharing info for images GTAO writes on the
    // compute queue and mesh.frag reads on graphics. Null on devices
    // without async compute — AO images fall back to exclusive sharing.
    const uint32_t* concurrent_families_{nullptr};
    uint32_t concurrent_family_count_{0};

    VkQueue init_graphics_queue_{VK_NULL_HANDLE};
    VkCommandPool init_graphics_pool_{VK_NULL_HANDLE};

private:
    VkSampler ao_sampler_{VK_NULL_HANDLE};
    VkDescriptorSetLayout blur_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout blur_layout_{VK_NULL_HANDLE};
    VkPipeline blur_pipeline_{VK_NULL_HANDLE};

    float radius_{1.0F};
    float intensity_{1.5F};
    float thickness_{0.5F};
    bool enabled_{true};

    Images default_images_;
};

// GTAO — "Ground-Truth Ambient Occlusion", horizon-based screen-space AO.
class GtaoTracePass : public AoPass {
public:
    [[nodiscard]] PassSetup setup() const override;

protected:
    Result<> create_trace(Device& device, const PassResources& resources) override;
    void declare_trace(PassBuilder& builder, const DeclareContext& ctx) override;
    void record_trace(const FrameContext& ctx, Images& images) override;
    void destroy_trace() override;

private:
    VkDescriptorSetLayout trace_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout trace_layout_{VK_NULL_HANDLE};
    VkPipeline trace_pipeline_{VK_NULL_HANDLE};
};

// Horizontal blur dispatch. Reads ao_image, writes transient gtao_blur.
class AoBlurHPass : public RenderPass {
public:
    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void post_create(GpuContext& gpu, PassRegistry& passes) override;

private:
    AoPass* owner_{nullptr};
};

// Vertical blur dispatch. Reads transient gtao_blur, writes ao_image.
class AoBlurVPass : public RenderPass {
public:
    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void post_create(GpuContext& gpu, PassRegistry& passes) override;

private:
    AoPass* owner_{nullptr};
};

} // namespace fjell
