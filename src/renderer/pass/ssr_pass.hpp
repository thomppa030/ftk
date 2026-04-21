#pragma once

#include "renderer/pass/render_pass.hpp"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>

namespace fjell {

// ── Screen-space reflections pipeline ───────────────────────────────────────
//
// SSR runs two compute dispatches authored as two independent DAG nodes:
//
//   ssr_trace     — hiz + prev-frame color → ssr_output.
//   ssr_composite — ssr_output + MR → scene color.
//
// With each dispatch as its own RenderPass, the FrameGraph emits one
// merged barrier per image per node and ssr_output's lifetime collapses
// to [trace..composite]. The win is small (the monolithic pass already
// kept ssr_output live across only two pass-boundaries), but the split
// keeps us consistent with the rest of the post-process chain.
//
// Layering:
//   • SsrCore          — abstract base. Owns both samplers, both
//                        descriptor layouts, both pipelines, the SSR
//                        parameters, and the cross-frame prev_frame
//                        history buffer (plus its ensure/copy helpers
//                        that run in pre_record/post_record).
//   • SsrTracePass     — concrete SsrCore. Registered as "ssr_trace".
//                        Records the trace dispatch or, when SSR is
//                        disabled, clears ssr_output to black so down-
//                        stream reads return zero.
//   • SsrCompositePass — follower. Holds SsrCore* set in post_create()
//                        via passes.get<SsrCore>("ssr_trace"). Records
//                        the composite dispatch.
//
// Resource flow:
//   • ssr_output is a graph transient — trace create()s it, composite
//     create()s it again for the dedup-by-name read.
//   • prev_frame_color stays pass-private (persistent cross-frame
//     history) and is exposed through ResourceRegistry::register_image
//     at frame start, same as today.
//
// Barrier contract: nodes declare only the accesses they perform. The
// graph emits the trace→composite layout transition and the disabled-
// branch TRANSFER_DST → SHADER_READ transition (as a final_layout on
// the trace node when it takes the clear path).

class SsrCore : public RenderPass {
public:
    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void register_resources(ResourceRegistry& registry) override;
    void pre_record(const FrameContext& ctx, GpuContext& gpu) override;
    void post_record(const FrameContext& ctx) override;

    // Previous-frame color buffer management (for SSR history).
    void ensure_prev_frame(VkDevice device, VmaAllocator allocator,
                            VkCommandPool command_pool, VkQueue queue,
                            uint32_t width, uint32_t height);
    void copy_prev_frame(VkCommandBuffer cmd, VkImage src_color, VkExtent2D extent);
    [[nodiscard]] VkImageView prev_frame_view() const { return prev_frame_view_; }

    void set_enabled(bool e) { enabled_ = e; }
    void set_max_steps(uint32_t s) { max_steps_ = s; }
    void set_max_distance(float d) { max_distance_ = d; }
    void set_thickness(float t) { thickness_ = t; }
    void set_intensity(float i) { intensity_ = i; }
    [[nodiscard]] bool enabled() const { return enabled_; }
    [[nodiscard]] uint32_t max_steps() const { return max_steps_; }
    [[nodiscard]] float max_distance() const { return max_distance_; }
    [[nodiscard]] float thickness() const { return thickness_; }
    [[nodiscard]] float intensity() const { return intensity_; }

    // Getters for follower passes.
    [[nodiscard]] VkSampler linear_sampler() const { return linear_sampler_; }
    [[nodiscard]] VkSampler nearest_sampler() const { return nearest_sampler_; }
    [[nodiscard]] VkDescriptorSetLayout trace_set_layout() const { return trace_set_layout_; }
    [[nodiscard]] VkDescriptorSetLayout composite_set_layout() const { return composite_set_layout_; }
    [[nodiscard]] VkPipelineLayout trace_pipeline_layout() const { return trace_pipeline_layout_; }
    [[nodiscard]] VkPipelineLayout composite_pipeline_layout() const { return composite_pipeline_layout_; }
    [[nodiscard]] VkPipeline trace_pipeline() const { return trace_pipeline_; }
    [[nodiscard]] VkPipeline composite_pipeline() const { return composite_pipeline_; }

protected:
    VkDevice device_{VK_NULL_HANDLE};

private:
    VkPipelineCache vk_pipeline_cache_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};

    VkSampler linear_sampler_{VK_NULL_HANDLE};
    VkSampler nearest_sampler_{VK_NULL_HANDLE};

    VkDescriptorSetLayout trace_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout trace_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline trace_pipeline_{VK_NULL_HANDLE};

    VkDescriptorSetLayout composite_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout composite_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline composite_pipeline_{VK_NULL_HANDLE};

    bool enabled_{true};
    uint32_t max_steps_{64};
    float max_distance_{100.0f};
    float thickness_{0.3f};
    float intensity_{1.0f};

    VkImage prev_frame_image_{VK_NULL_HANDLE};
    VmaAllocation prev_frame_allocation_{VK_NULL_HANDLE};
    VkImageView prev_frame_view_{VK_NULL_HANDLE};
    VkExtent2D prev_frame_extent_{};
};

class SsrTracePass : public SsrCore {
public:
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
};

class SsrCompositePass : public RenderPass {
public:
    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void post_create(GpuContext& gpu, PassRegistry& passes) override;

private:
    SsrCore* core_{nullptr};
};

// Deprecated alias — stats_panel and other callers still reach in via
// `ssr_pass()` with type SsrPass. Point them at the abstract core.
using SsrPass = SsrCore;

} // namespace fjell
