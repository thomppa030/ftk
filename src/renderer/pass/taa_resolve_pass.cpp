#include "renderer/pass/taa_resolve_pass.hpp"

#include "core/log.hpp"
#include "core/profiler.hpp"
#include "gpu/device.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/post_process_resources.hpp"

namespace fjell {

// ── Images (per-viewport state) ─────────────────────────────────────────────

void TaaResolvePass::Images::create(gpu::Device& gpu, uint32_t width, uint32_t height) {
    extent = {width, height};
    write_index = 0;
    needs_init = true;
    for (auto& slot : history) {
        auto made = gpu.create(gpu::TextureDesc{
            .format = gpu::Format::rgba16_float,
            .width = width,
            .height = height,
            .use = gpu::TextureUse::storage | gpu::TextureUse::sampled,
            .name = "taa_history",
        });
        if (!made) {
            FJELL_GFX_ERROR("{}", made.error());
            destroy();
            return;
        }
        slot = std::move(*made);
    }
}

void TaaResolvePass::Images::destroy() {
    for (auto& slot : history) slot.reset();
    extent = {0, 0};
}

void TaaResolvePass::Images::resize(gpu::Device& gpu, uint32_t width, uint32_t height) {
    if (extent.x == width && extent.y == height) return;
    destroy();
    if (width > 0 && height > 0) create(gpu, width, height);
}

// ── TaaResolvePass (singleton) ─────────────────────────────────────────────

Result<> TaaResolvePass::create(Device& /*device*/, const PassResources& resources) {
    gpu::Device& device = *resources.device;
    sampler_ = device.sampler({.filter = gpu::Filter::linear,
                               .mip_filter = gpu::Filter::nearest,
                               .address = gpu::Address::clamp});
    FJELL_TRY(pipeline_, device.create(gpu::ComputePipelineDesc{
                             .shader = "shaders/taa_resolve.comp",
                             .name = "taa_resolve",
                         }));
    FJELL_GFX_DEBUG("TAA resolve pass created (compute pipeline)");
    return {};
}

void TaaResolvePass::destroy() {
    pipeline_.reset();
}

PassSetup TaaResolvePass::setup() const {
    return {
        .name = "taa_resolve",
        .phase = RenderPhase::PostProcess,
        .sub_order = 150, // after the fog composite (104), before bloom (200)
    };
}

void TaaResolvePass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    auto color = builder.import_named(ctx, "color");
    auto velocity = builder.import_named(ctx, "velocity");
    auto depth = builder.import_named(ctx, "depth");
    builder.read(color, gpu::Access::sampled_compute);
    builder.read(velocity, gpu::Access::sampled_compute);
    builder.read(depth, gpu::Access::sampled_compute);
    // Blends into this frame's history slot from last frame's. The slots
    // are persistent, which keeps the pass alive for the frame after even
    // when nothing reads its output now.
    builder.read(builder.import_named(ctx, "taa_history_read"), gpu::Access::sampled_compute);
    builder.write(builder.import_named(ctx, "taa_history_write"),
                  gpu::Access::storage_write_compute);
}

void TaaResolvePass::record(const FrameContext& ctx) {
    FJELL_PROFILE_SCOPE_N("record_taa");
    // Only a viewport's own history is declared to the graph, which is what
    // moves it between layouts; without one there is nothing to write into.
    auto* imgs = ctx.pp_resources ? ctx.pp_resources->taa() : nullptr;
    if (imgs == nullptr) { return; }
    record_into(ctx, *imgs);
}

void TaaResolvePass::record_into(const FrameContext& ctx, Images& imgs) {
    if (imgs.extent.x == 0 || !ctx.target) return;
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("TAA Resolve");

    const uint32_t write_idx = imgs.write_index;
    const uint32_t read_idx = 1 - write_idx;

    // The first frame after the history is made has nothing to blend
    // from: the read slot holds nothing yet, so the output is this frame's
    // colour alone. The graph has already moved both slots into the layouts
    // declared for them.
    const bool first_frame = imgs.needs_init;
    imgs.needs_init = false;

    struct TaaPush {
        float texel_x, texel_y;
        float blend_factor;
        float pad;
    };
    cmd.set_pipeline(pipeline_);
    cmd.bind({{"current_color", gpu::sampled(ctx.target->color_texture(), sampler_)},
              {"history_color", gpu::sampled(imgs.history[read_idx], sampler_)},
              {"velocity_tex", gpu::sampled(ctx.target->velocity_texture(), sampler_)},
              {"resolve_output", gpu::storage(imgs.history[write_idx])},
              {"depth_tex", gpu::sampled(ctx.target->depth_texture(), sampler_)}});
    cmd.push(TaaPush{
        .texel_x = 1.0f / static_cast<float>(imgs.extent.x),
        .texel_y = 1.0f / static_cast<float>(imgs.extent.y),
        .blend_factor = first_frame ? 1.0f : 0.05f,
        .pad = 0.0f,
    });
    cmd.dispatch((imgs.extent.x + 7) / 8, (imgs.extent.y + 7) / 8, 1);

    // Flip ping-pong
    imgs.write_index = read_idx;
}

} // namespace fjell
