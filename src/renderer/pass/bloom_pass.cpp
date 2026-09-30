// ── Bloom downsample/upsample pipeline implementation ──────────────────────
//
// Nine RenderPass classes, one fullscreen draw each. Shared sampler +
// pipelines live on BloomCore (BloomDs0Pass IS the core); follower ds/us
// nodes hold a BloomCore* looked up in post_create() via passes.get. Each
// node declares only the mip slots it touches, so the graph's RDG-style
// merged-barrier emitter can per-node-barrier the mip chain, and each
// mip's lifetime is short enough that mip 0 and mip 4 (plus mip 1 and
// mip 3) alias in the transient pool.

#include "renderer/pass/bloom_pass.hpp"

#include "core/log.hpp"
#include "core/profiler.hpp"
#include "gpu/device.hpp"
#include "gpu/render_encoder.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/pass_registry.hpp"

#include <algorithm>
#include <string>

namespace fjell {

namespace {

constexpr gpu::Format BLOOM_FORMAT = gpu::Format::rgba16_float;

std::string mip_name(uint32_t idx) {
    return "bloom_mip" + std::to_string(idx);
}

struct DownsamplePush {
    float threshold;
    float is_first;
    float pad1;
    float pad2;
};

struct UpsamplePush {
    float blend;
    float pad1;
    float pad2;
    float pad3;
};

// A mip of the chain this frame and the size the graph made it.
struct Mip {
    gpu::Texture texture{};
    uint32_t width{0};
    uint32_t height{0};
};

Mip resolve_mip(const FrameContext& ctx, uint32_t idx) {
    if (ctx.target == nullptr) { return {}; }
    const gpu::Texture texture = ctx.texture(mip_name(idx));
    if (!texture.valid()) { return {}; }
    const auto extent = ctx.target->extent();
    const uint32_t div = 1U << (idx + 1);
    return {texture, std::max(1U, extent.x / div), std::max(1U, extent.y / div)};
}

// One fullscreen draw into `dst`, sampling `source`.
template <typename Push>
void draw_mip(const FrameContext& ctx, gpu::GraphicsPipeline pipeline, gpu::Sampler sampler,
              gpu::Texture source, const Mip& dst, gpu::Load load, const Push& push) {
    auto pass = ctx.commands().render({
        .color = {{.view = dst.texture, .load = load}},
        .width = dst.width,
        .height = dst.height,
    });
    pass.set_pipeline(pipeline);
    pass.bind({{"source_tex", gpu::sampled(source, sampler)}});
    pass.push(push);
    pass.draw(3);
}

void declare_mip(PassBuilder& builder, uint32_t idx, gpu::Access access) {
    TextureDesc desc{};
    desc.format = BLOOM_FORMAT;
    desc.size_class = SizeClass::viewport;
    desc.viewport_divisor = 1U << (idx + 1);
    auto h = builder.create(mip_name(idx), desc);
    if (access == gpu::Access::color_attachment) {
        builder.write(h, access);
    } else {
        builder.read(h, access);
    }
}

void declare_mip_read_write(PassBuilder& builder, uint32_t idx) {
    TextureDesc desc{};
    desc.format = BLOOM_FORMAT;
    desc.size_class = SizeClass::viewport;
    desc.viewport_divisor = 1U << (idx + 1);
    auto h = builder.create(mip_name(idx), desc);
    builder.read(h, gpu::Access::color_attachment);
    builder.write(h, gpu::Access::color_attachment);
}

} // namespace

// ── BloomCore ───────────────────────────────────────────────────────────────

Result<> BloomCore::create(const PassResources& resources) {
    gpu::Device& device = *resources.device;
    sampler_ = device.sampler({.filter = gpu::Filter::linear,
                               .mip_filter = gpu::Filter::nearest,
                               .address = gpu::Address::clamp});
    FJELL_TRY(downsample_pipeline_, device.create(gpu::GraphicsPipelineDesc{
                                        .vertex = "shaders/depth_viz.vert",
                                        .fragment = "shaders/bloom_downsample.frag",
                                        .color = {{BLOOM_FORMAT, gpu::Blend::none}},
                                        .name = "bloom_downsample",
                                    }));
    // Each upsample adds the coarser mip into the finer one.
    FJELL_TRY(upsample_pipeline_, device.create(gpu::GraphicsPipelineDesc{
                                      .vertex = "shaders/depth_viz.vert",
                                      .fragment = "shaders/bloom_upsample.frag",
                                      .color = {{BLOOM_FORMAT, gpu::Blend::additive}},
                                      .name = "bloom_upsample",
                                  }));
    FJELL_GFX_DEBUG("Bloom core created");
    return {};
}

void BloomCore::destroy() {
    downsample_pipeline_.reset();
    upsample_pipeline_.reset();
}

// ── BloomDs0Pass ────────────────────────────────────────────────────────────

PassSetup BloomDs0Pass::setup() const {
    return {
        .name = "bloom_ds_0",
        .phase = RenderPhase::PostProcess,
        .sub_order = 200,
    };
}

void BloomDs0Pass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    if (!enabled()) { return; }

    auto color = builder.import_named(ctx, "color");
    builder.read(color, gpu::Access::sampled_fragment);
    declare_mip(builder, 0, gpu::Access::color_attachment);
}

void BloomDs0Pass::record(const FrameContext& ctx) {
    if (!enabled() || ctx.target == nullptr) { return; }

    const Mip dst = resolve_mip(ctx, 0);
    if (!dst.texture.valid()) { return; }

    FJELL_PROFILE_SCOPE_N("record_bloom_ds0");
    auto zone = ctx.commands().zone("Bloom Ds0");
    draw_mip(ctx, downsample_pipeline(), sampler(), ctx.target->color_texture(), dst,
             gpu::Load::discard, DownsamplePush{threshold(), 1.0F, 0.0F, 0.0F});
}

// ── BloomDsPass (mip_index 1..4) ────────────────────────────────────────────

Result<> BloomDsPass::create(const PassResources& /*resources*/) {
    return {};
}

void BloomDsPass::destroy() {}

void BloomDsPass::post_create(GpuContext& /*gpu*/, PassRegistry& passes) {
    core_ = &passes.get<BloomCore>("bloom_ds_0");
}

PassSetup BloomDsPass::setup() const {
    return {
        .name = "bloom_ds_" + std::to_string(mip_index_),
        .phase = RenderPhase::PostProcess,
        .sub_order = 200 + mip_index_,
    };
}

void BloomDsPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    (void)ctx;
    if (core_ == nullptr || !core_->enabled()) { return; }

    declare_mip(builder, mip_index_ - 1, gpu::Access::sampled_fragment);
    declare_mip(builder, mip_index_, gpu::Access::color_attachment);
}

void BloomDsPass::record(const FrameContext& ctx) {
    if (core_ == nullptr || !core_->enabled()) { return; }

    const Mip src = resolve_mip(ctx, mip_index_ - 1);
    const Mip dst = resolve_mip(ctx, mip_index_);
    if (!src.texture.valid() || !dst.texture.valid()) { return; }

    FJELL_PROFILE_SCOPE_N("record_bloom_ds");
    auto zone = ctx.commands().zone(zone_name_);
    draw_mip(ctx, core_->downsample_pipeline(), core_->sampler(), src.texture, dst,
             gpu::Load::discard, DownsamplePush{core_->threshold(), 0.0F, 0.0F, 0.0F});
}

// ── BloomUsPass (dst_mip 3..0) ──────────────────────────────────────────────

Result<> BloomUsPass::create(const PassResources& /*resources*/) {
    return {};
}

void BloomUsPass::destroy() {}

void BloomUsPass::post_create(GpuContext& /*gpu*/, PassRegistry& passes) {
    core_ = &passes.get<BloomCore>("bloom_ds_0");
}

PassSetup BloomUsPass::setup() const {
    // Upsample runs after downsample. sub_order offset keeps the us_3..us_0
    // order stable and places all upsamples after all downsamples within
    // the PostProcess phase (downsamples use 200..204).
    return {
        .name = "bloom_us_" + std::to_string(dst_mip_),
        .phase = RenderPhase::PostProcess,
        .sub_order = 210 + (BLOOM_MIP_COUNT - 2 - dst_mip_),
    };
}

void BloomUsPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    (void)ctx;
    if (core_ == nullptr || !core_->enabled()) { return; }

    declare_mip(builder, dst_mip_ + 1, gpu::Access::sampled_fragment);
    declare_mip_read_write(builder, dst_mip_);
    // No final_layout/has_side_effects on dst_mip 0: the post pass declares a
    // read on "bloom_mip0" (when bloom is enabled), so the DAG sees the
    // consumer edge and emits the COLOR_ATTACHMENT → SHADER_READ barrier
    // between this pass and the post pass automatically.
}

void BloomUsPass::record(const FrameContext& ctx) {
    if (core_ == nullptr || !core_->enabled()) { return; }

    const Mip src = resolve_mip(ctx, dst_mip_ + 1);
    const Mip dst = resolve_mip(ctx, dst_mip_);
    if (!src.texture.valid() || !dst.texture.valid()) { return; }

    FJELL_PROFILE_SCOPE_N("record_bloom_us");
    auto zone = ctx.commands().zone(zone_name_);
    draw_mip(ctx, core_->upsample_pipeline(), core_->sampler(), src.texture, dst,
             gpu::Load::load, UpsamplePush{0.6F, 0.0F, 0.0F, 0.0F});
}

} // namespace fjell
