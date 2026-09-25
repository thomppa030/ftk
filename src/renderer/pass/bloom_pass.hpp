#pragma once

#include "renderer/pass/render_pass.hpp"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>

namespace fjell {

// ── Bloom downsample/upsample pipeline ──────────────────────────────────────
//
// Bloom runs 9 full-screen draws authored as 9 independent DAG nodes:
//
//   bloom_ds_0 .. bloom_ds_4  — downsample chain, each writes one mip and
//                                reads either scene color (ds_0) or the
//                                previous mip (ds_1..4).
//   bloom_us_3 .. bloom_us_0  — upsample chain, each reads mip[i+1] and
//                                additively blends into mip[i].
//
// With each draw as its own RenderPass, the FrameGraph emits one merged
// barrier per image per node and the mip chain's bytes aren't held hostage
// by a single monolithic [N..N] lifetime — mip[0] and mip[4] never coexist
// so they can alias, same for mip[1]/mip[3].
//
// Layering:
//   • BloomCore     — abstract base. Owns shared sampler, descriptor set
//                     layouts, pipeline layouts, pipelines, and the
//                     enabled/threshold/intensity knobs. Itself a
//                     RenderPass; subclass runs downsample of mip 0.
//   • BloomDs0Pass  — concrete BloomCore. Registered as "bloom_ds_0".
//                     Runs downsample dispatch on scene color → mip[0].
//   • BloomDsPass   — follower for ds_1..4. Holds BloomCore* set in
//                     post_create() via passes.get<BloomCore>("bloom_ds_0").
//                     Records one downsample dispatch.
//   • BloomUsPass   — follower for us_3..0. Reads mip[dst+1], blends into
//                     mip[dst]. dst=0 is the final node — it claims
//                     final_layout(SHADER_READ_ONLY) and flags
//                     has_side_effects() so the graph keeps it alive
//                     (the post pass samples bloom_mip0 through the registry
//                     at record time, so the DAG can't see the reader).
//
// Resource flow:
//   • bloom_mip0..4 are graph transients declared by the ds nodes. Each
//     upsample node re-create()s the mip it reads + the mip it writes so
//     the graph dedupes by name and sees the producer→consumer edges.
//   • No bloom_output: the monolithic pass used to composite scene+mip0
//     into an unused buffer; the post pass samples mip0 directly.
//
// Barrier contract: each node declares only the reads/writes it actually
// performs. The graph emits one merged barrier per image per node. No node
// emits inline barriers — the final mip0 layout is carried by us_0's
// final_layout(SHADER_READ_ONLY) promise so the graph transitions it on
// the next read.

constexpr uint32_t BLOOM_MIP_COUNT = 5;

class BloomCore : public RenderPass {
public:
    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;

    void set_threshold(float t) { threshold_ = t; }
    void set_intensity(float i) { intensity_ = i; }
    void set_enabled(bool e) { enabled_ = e; }
    [[nodiscard]] float threshold() const { return threshold_; }
    [[nodiscard]] float intensity() const { return intensity_; }
    [[nodiscard]] bool enabled() const { return enabled_; }

    [[nodiscard]] VkSampler sampler() const { return sampler_; }
    [[nodiscard]] VkDescriptorSetLayout single_tex_layout() const { return single_tex_layout_; }
    [[nodiscard]] VkPipelineLayout downsample_layout() const { return downsample_layout_; }
    [[nodiscard]] VkPipelineLayout upsample_layout() const { return upsample_layout_; }
    [[nodiscard]] VkPipeline downsample_pipeline() const { return downsample_pipeline_; }
    [[nodiscard]] VkPipeline upsample_pipeline() const { return upsample_pipeline_; }

private:
    VkDevice device_{VK_NULL_HANDLE};
    VkPipelineCache vk_pipeline_cache_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};

    VkSampler sampler_{VK_NULL_HANDLE};
    VkDescriptorSetLayout single_tex_layout_{VK_NULL_HANDLE};

    VkPipelineLayout downsample_layout_{VK_NULL_HANDLE};
    VkPipeline downsample_pipeline_{VK_NULL_HANDLE};
    VkPipelineLayout upsample_layout_{VK_NULL_HANDLE};
    VkPipeline upsample_pipeline_{VK_NULL_HANDLE};

    float threshold_{1.0F};
    float intensity_{0.25F};
    bool enabled_{true};
};

// Concrete ds_0 — registered under "bloom_ds_0", owns BloomCore's shared
// state. Records downsample(scene_color) → bloom_mip0.
class BloomDs0Pass : public BloomCore {
public:
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
};

// Follower downsample node for mip_index 1..4. Reads bloom_mip<idx-1>,
// writes bloom_mip<idx>.
class BloomDsPass : public RenderPass {
public:
    explicit BloomDsPass(uint32_t mip_index) : mip_index_(mip_index) {}

    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void post_create(GpuContext& gpu, PassRegistry& passes) override;

private:
    uint32_t mip_index_{0};
    BloomCore* core_{nullptr};
};

// Upsample node: reads bloom_mip<dst+1>, additively blends into
// bloom_mip<dst>. dst=0 claims the final_layout and flags
// has_side_effects() since the post pass's consumption isn't visible to the DAG.
class BloomUsPass : public RenderPass {
public:
    explicit BloomUsPass(uint32_t dst_mip) : dst_mip_(dst_mip) {}

    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void post_create(GpuContext& gpu, PassRegistry& passes) override;

private:
    uint32_t dst_mip_{0};
    BloomCore* core_{nullptr};
};

} // namespace fjell
