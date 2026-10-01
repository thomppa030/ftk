// ── Screen-space reflections pipeline implementation ───────────────────────
//
// Three RenderPass classes. SsrTracePass IS the SsrCore (owns samplers,
// pipelines, prev-frame history); SsrCompositePass and SsrHistoryPass are
// followers holding SsrCore* looked up in post_create(). Each node declares
// only the accesses it performs, and ssr_output is a graph transient deduped
// by name across the trace and the composite.

#include "renderer/pass/ssr_pass.hpp"

#include "ftk/base/log.hpp"
#include "ftk/base/profiler.hpp"
#include "ftk/framegraph/pass_builder.hpp"
#include "ftk/framegraph/resource_registry.hpp"
#include "ftk/gpu/device.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass_registry.hpp"
#include "renderer/post_process_resources.hpp"

#include <cstdlib>

namespace fjell {

namespace {

constexpr uint32_t TILE_SIZE = 8;

struct TracePushConstants {
    uint32_t viewport_width;
    uint32_t viewport_height;
    uint32_t hiz_mip_count;
    uint32_t max_steps;
    float max_distance;
    float thickness;
    float intensity;
    float pad;
};

struct CompositePushConstants {
    uint32_t viewport_width;
    uint32_t viewport_height;
    float intensity;
    uint32_t source; // 0 = ssr, 1 = rt
    uint32_t passthrough;
};

} // namespace

// ── SsrCore ─────────────────────────────────────────────────────────────────

Result<> SsrCore::create(const PassResources& resources) {
    device_ = resources.device;
    nearest_sampler_ = device_->sampler({.filter = gpu::Filter::nearest,
                                         .mip_filter = gpu::Filter::nearest,
                                         .address = gpu::Address::clamp});
    linear_sampler_ = device_->sampler({.filter = gpu::Filter::linear,
                                        .mip_filter = gpu::Filter::nearest,
                                        .address = gpu::Address::clamp});
    FJELL_TRY(trace_pipeline_, device_->create(gpu::ComputePipelineDesc{
                                   .shader = "shaders/ssr_trace.comp",
                                   .shared = {resources.globals},
                                   .name = "ssr_trace",
                               }));
    FJELL_TRY(composite_pipeline_, device_->create(gpu::ComputePipelineDesc{
                                       .shader = "shaders/ssr_composite.comp",
                                       .name = "ssr_composite",
                                   }));
    FJELL_GFX_DEBUG("SSR pass created");
    return {};
}

void SsrCore::destroy() {
    prev_frame_.reset();
    prev_frame_extent_ = {0, 0};
    trace_pipeline_.reset();
    composite_pipeline_.reset();
}

void SsrCore::ensure_prev_frame(uint32_t width, uint32_t height) {
    if (prev_frame_ && width == prev_frame_extent_.x && height == prev_frame_extent_.y) return;
    // Black until the first copy, so the first frame's trace reads nothing.
    auto made = device_->create(gpu::TextureDesc{
        .format = gpu::Format::rgba16_float,
        .width = width,
        .height = height,
        .use = gpu::TextureUse::sampled,
        .initial = gpu::Clear{0.0f, 0.0f, 0.0f, 0.0f},
        .name = "ssr_prev_frame",
    });
    if (!made) {
        FJELL_GFX_ERROR("{}", made.error());
        prev_frame_.reset();
        prev_frame_extent_ = {0, 0};
        return;
    }
    prev_frame_ = std::move(*made);
    prev_frame_extent_ = {width, height};
}

void SsrCore::copy_prev_frame(gpu::CommandList& cmd, gpu::Texture color, glm::uvec2 extent) {
    if (prev_frame_ && prev_frame_extent_ == extent) cmd.copy(color, prev_frame_);
}

void SsrCore::pre_record(const FrameContext& ctx, GpuContext& /*gpu*/) {
    const bool in_graph = (ctx.pp_resources != nullptr) && ctx.pp_resources->config().ssr;
    if (in_graph && screen_space_active() && ctx.target != nullptr) {
        const auto ext = ctx.target->extent();
        ensure_prev_frame(ext.x, ext.y);
    }
}

void SsrCore::collect_exports(DeclareContext& ctx) {
    if (!prev_frame_) { return; }
    // Read by the next frame's trace, and rests where it samples it.
    ctx.export_texture("prev_frame_color", prev_frame_,
                       {.persistent = true, .resting = gpu::Access::sampled_compute});
}

// ── SsrTracePass ────────────────────────────────────────────────────────────

PassSetup SsrTracePass::setup() const {
    return {
        .name = "ssr_trace",
        .phase = RenderPhase::PostProcess,
        .sub_order = 0,
    };
}

void SsrTracePass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    auto depth = builder.import_named(ctx, "depth");
    builder.read(depth, gpu::Access::sampled_compute);

    TextureDesc ssr_desc{};
    ssr_desc.format = gpu::Format::rgba16_float;
    ssr_desc.size_class = SizeClass::viewport;
    auto ssr_out = builder.create("ssr_output", ssr_desc);

    if (screen_space_active()) {
        builder.write(ssr_out, gpu::Access::storage_write_compute);
        // The trace steps through the Hi-Z pyramid and takes the colour it
        // hits from the last frame's.
        if (auto hiz = builder.import_named_optional(ctx, "hiz_pyramid"); hiz.valid()) {
            builder.read(hiz, gpu::Access::sampled_compute);
        }
        if (auto history = builder.import_named_optional(ctx, "prev_frame_color");
            history.valid()) {
            builder.read(history, gpu::Access::sampled_compute);
        }
    } else {
        // Off or ray traced: ssr_output is cleared so the composite reads
        // zero from it. (When ray traced, the composite reads
        // rt_reflection_output instead.)
        builder.write(ssr_out, gpu::Access::clear);
    }
}

void SsrTracePass::record(const FrameContext& ctx) {
    const gpu::Texture ssr_out = ctx.texture("ssr_output");
    if (!ssr_out.valid()) { return; }

    FJELL_PROFILE_SCOPE_N("record_ssr_trace");
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("SSR Trace");

    if (!screen_space_active()) {
        cmd.clear(ssr_out, gpu::Clear{0.0f, 0.0f, 0.0f, 0.0f});
        return;
    }

    const auto* hiz = ctx.registry->find_image("hiz");
    if (!prev_frame().valid()) { return; }
    if (hiz == nullptr || !hiz->texture.valid() || hiz->mip_count == 0) { return; }

    const auto extent = ctx.target->extent();
    cmd.set_pipeline(trace_pipeline());
    cmd.bind(ctx.globals);
    cmd.bind({{"hiz_texture", gpu::sampled(hiz->texture, nearest_sampler())},
              {"prev_color", gpu::sampled(prev_frame(), linear_sampler())},
              {"ssr_output", gpu::storage(ssr_out)}});
    cmd.push(TracePushConstants{
        .viewport_width = extent.x,
        .viewport_height = extent.y,
        .hiz_mip_count = hiz->mip_count,
        .max_steps = max_steps(),
        .max_distance = max_distance(),
        .thickness = thickness(),
        .intensity = intensity(),
        .pad = 0.0f,
    });
    cmd.dispatch((extent.x + TILE_SIZE - 1) / TILE_SIZE,
                 (extent.y + TILE_SIZE - 1) / TILE_SIZE, 1);
}

// ── SsrCompositePass ────────────────────────────────────────────────────────

Result<> SsrCompositePass::create(const PassResources& /*resources*/) {
    return {};
}

void SsrCompositePass::destroy() {}

void SsrCompositePass::post_create(GpuContext& /*gpu*/, PassRegistry& passes) {
    core_ = &passes.get<SsrCore>("ssr_trace");
}

PassSetup SsrCompositePass::setup() const {
    return {
        .name = "ssr_composite",
        .phase = RenderPhase::PostProcess,
        .sub_order = 1,
    };
}

void SsrCompositePass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    if (core_ == nullptr) { return; }

    // ssr_output is a framegraph transient (created/cleared by SsrTracePass).
    // rt_reflection_output is each viewport's own, exported where it has one;
    // it rests where this samples it, so it is readable whether or not the
    // trace ran this frame.
    TextureDesc refl_desc{};
    refl_desc.format = gpu::Format::rgba16_float;
    refl_desc.size_class = SizeClass::viewport;
    auto ssr_out = builder.create("ssr_output", refl_desc);
    builder.read(ssr_out, gpu::Access::sampled_compute);

    if (auto rt = builder.import_named_optional(ctx, RtReflectionPass::OUTPUT_NAME); rt.valid()) {
        builder.read(rt, gpu::Access::sampled_compute);
    }

    auto color = builder.import_named(ctx, "color");
    auto mr = builder.import_named(ctx, "mr");
    builder.read_write(color, gpu::Access::storage_read_write_compute);
    builder.read(mr, gpu::Access::sampled_compute);
}

void SsrCompositePass::record(const FrameContext& ctx) {
    if (core_ == nullptr) { return; }
    // When mode != ScreenSpace, SsrTracePass clears ssr_output to black so
    // the composite reads zero from this branch. Composite still runs and
    // blends in whichever reflection source matches the active mode.
    const gpu::Texture ssr = ctx.texture("ssr_output");
    const gpu::Texture mr = ctx.texture("mr");
    if (!ssr.valid() || !mr.valid()) { return; }

    // A viewport without ray-traced reflections of its own has no output to
    // read. The binding falls back to ssr_output, which in RT mode reads
    // black.
    const auto* rt_images = ctx.pp_resources != nullptr ? ctx.pp_resources->rt_reflection() : nullptr;
    gpu::Texture rt = rt_images != nullptr ? rt_images->output.get() : gpu::Texture{};
    const bool rt_ready = rt.valid();
    if (!rt_ready) rt = ssr;

    FJELL_PROFILE_SCOPE_N("record_ssr_composite");
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("SSR Composite");

    // A denoise debug view writes diagnostic colours into the reflection
    // buffer; show them as-is rather than through the Fresnel response, which
    // would scale them by 0.04 on a dielectric and leave them invisible.
    static const bool debug_passthrough = [] {
        const char* v = std::getenv("FJELL_RT_DENOISE_DEBUG");
        return v != nullptr && v[0] != '0' && v[0] != '\0';
    }();

    const auto extent = ctx.target->extent();
    const gpu::Sampler nearest = core_->nearest_sampler();
    cmd.set_pipeline(core_->composite_pipeline());
    cmd.bind({{"ssr_texture", gpu::sampled(ssr, nearest)},
              {"scene_color", gpu::storage(ctx.target->color_texture())},
              {"mr_texture", gpu::sampled(mr, nearest)},
              {"rt_reflection_texture", gpu::sampled(rt, nearest)}});
    cmd.push(CompositePushConstants{
        .viewport_width = extent.x,
        .viewport_height = extent.y,
        .intensity = core_->intensity(),
        .source = (core_->ray_traced_active() && rt_ready) ? 1U : 0U,
        .passthrough = debug_passthrough ? 1U : 0U,
    });
    cmd.dispatch((extent.x + TILE_SIZE - 1) / TILE_SIZE,
                 (extent.y + TILE_SIZE - 1) / TILE_SIZE, 1);
}

// ── SsrHistoryPass ──────────────────────────────────────────────────────────

Result<> SsrHistoryPass::create(const PassResources& /*resources*/) {
    return {};
}

void SsrHistoryPass::destroy() {}

void SsrHistoryPass::post_create(GpuContext& /*gpu*/, PassRegistry& passes) {
    core_ = &passes.get<SsrCore>("ssr_trace");
}

PassSetup SsrHistoryPass::setup() const {
    return {
        .name = "ssr_history",
        .phase = RenderPhase::PostProcess,
        .sub_order = 125,   // the colour is final once the shafts composite (122) has run
    };
}

void SsrHistoryPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    if (core_ == nullptr || !core_->screen_space_active()) { return; }
    auto history = builder.import_named_optional(ctx, "prev_frame_color");
    if (!history.valid()) { return; }
    builder.read(builder.import_named(ctx, "color"), gpu::Access::copy_src);
    builder.write(history, gpu::Access::copy_dst);
}

void SsrHistoryPass::record(const FrameContext& ctx) {
    if (core_ == nullptr || !core_->screen_space_active() || ctx.target == nullptr) { return; }
    FJELL_PROFILE_SCOPE_N("record_ssr_history");
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("SSR History");
    const auto extent = ctx.target->extent();
    core_->copy_prev_frame(cmd, ctx.target->color_texture(), {extent.x, extent.y});
}

} // namespace fjell
