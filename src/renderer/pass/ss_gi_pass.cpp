#include "renderer/pass/ss_gi_pass.hpp"

#include "ftk/base/log.hpp"
#include "ftk/base/profiler.hpp"
#include "ftk/framegraph/pass_builder.hpp"
#include "ftk/gpu/device.hpp"
#include "renderer/accel_structure.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass_registry.hpp"
#include "renderer/post_process_resources.hpp"
#include "renderer/resources/mega_buffer.hpp"
#include "scene/scene.hpp"

#include <array>
#include <optional>

namespace fjell {

namespace {

struct TracePush {
    uint32_t rays_per_probe;
    uint32_t ray_offset;
};

struct ResolvePush {
    glm::uvec2 probe_count;
    uint32_t rays_per_probe;
    uint32_t frame_index;
    glm::vec2 resolution;
    uint32_t probe_spacing;
    float hysteresis;
};

struct UpsamplePush {
    glm::uvec2 output_size;
    glm::uvec2 probe_count;
    uint32_t probe_spacing;
    float pad0;
};

// Where the probe textures rest between frames: sampled by the pass's own
// compute shaders. They are made cleared, which leaves them there.
constexpr ftk::gpu::Access REST = ftk::gpu::Access::sampled_compute;

} // namespace

// ── Images lifecycle ────────────────────────────────────────────────────

void SsGiPass::Images::create(ftk::gpu::Device& gpu, const SsGiConfig& config,
                              uint32_t screen_w, uint32_t screen_h) {
    probe_count = {
        (screen_w + config.probe_spacing - 1) / config.probe_spacing,
        (screen_h + config.probe_spacing - 1) / config.probe_spacing
    };
    rays_per_probe = config.rays_per_probe;
    indirect_extent = {std::max(1U, screen_w / 2), std::max(1U, screen_h / 2)};

    auto make = [&](glm::uvec2 size, ftk::gpu::TextureUses use, std::optional<ftk::gpu::Clear> initial,
                    std::string_view name) -> ftk::gpu::Owned<ftk::gpu::Texture> {
        auto made = gpu.create(ftk::gpu::TextureDesc{
            .format = ftk::gpu::Format::rgba16_float,
            .width = size.x,
            .height = size.y,
            .use = use,
            .initial = initial,
            .name = name,
        });
        if (!made) {
            FTK_GFX_ERROR("{}", made.error());
            return {};
        }
        return std::move(*made);
    };
    const ftk::gpu::TextureUses storage_sampled = ftk::gpu::TextureUse::storage | ftk::gpu::TextureUse::sampled;
    raw_radiance = make({probe_count.x, probe_count.y * rays_per_probe}, storage_sampled,
                        ftk::gpu::Clear{}, "ss_gi_raw_radiance");
    resolved = make(probe_count, storage_sampled, ftk::gpu::Clear{}, "ss_gi_resolved");
    history = make(probe_count, ftk::gpu::TextureUse::sampled, ftk::gpu::Clear{}, "ss_gi_history");
    // Exported to the frame graph, which starts it undefined until written.
    indirect = make(indirect_extent, storage_sampled, std::nullopt, "ss_gi_indirect");

    written = false;
    // A new history holds nothing to accumulate onto.
    history_valid = false;
}

void SsGiPass::Images::resize(ftk::gpu::Device& gpu, const SsGiConfig& config,
                              uint32_t screen_w, uint32_t screen_h) {
    if (screen_w == 0 || screen_h == 0) return;

    // Probe count drives the size of every probe texture, and the probe grid
    // is what maps screen pixels to probes — keeping a stale grid stretches it
    // across the new viewport and quantises the result into probe-sized blocks.
    const glm::uvec2 wanted_probes{
        (screen_w + config.probe_spacing - 1) / config.probe_spacing,
        (screen_h + config.probe_spacing - 1) / config.probe_spacing
    };
    const glm::uvec2 wanted_output{std::max(1U, screen_w / 2), std::max(1U, screen_h / 2)};
    if (indirect && wanted_probes == probe_count && wanted_output == indirect_extent
        && config.rays_per_probe == rays_per_probe) {
        return;
    }
    create(gpu, config, screen_w, screen_h);
}

void SsGiPass::Images::fit_prev_depth(ftk::gpu::Device& gpu, ftk::gpu::Texture scene_depth) {
    const ftk::gpu::TextureInfo want = gpu.info(scene_depth);
    if (prev_depth) {
        const ftk::gpu::TextureInfo& have = gpu.info(prev_depth);
        if (have.width == want.width && have.height == want.height && have.format == want.format) {
            return;
        }
    }
    auto made = gpu.create(ftk::gpu::TextureDesc{
        .format = want.format,
        .width = want.width,
        .height = want.height,
        .use = ftk::gpu::TextureUse::sampled,
        .name = "ss_gi_prev_depth",
    });
    if (!made) {
        FTK_GFX_ERROR("{}", made.error());
        prev_depth.reset();
        return;
    }
    prev_depth = std::move(*made);
    // It holds no depth yet, so nothing tells where the history is stale.
    history_valid = false;
}

void SsGiPass::Images::destroy() {
    raw_radiance.reset();
    resolved.reset();
    history.reset();
    indirect.reset();
    prev_depth.reset();
    indirect_extent = {0, 0};
    probe_count = {0, 0};
    written = false;
    history_valid = false;
}

// ── Pass create ─────────────────────────────────────────────────────────

ftk::Result<> SsGiPass::create(const PassResources& resources) {
    ftk::gpu::Device& device = *resources.device;
    linear_sampler_ = device.sampler({.filter = ftk::gpu::Filter::linear,
                                      .mip_filter = ftk::gpu::Filter::nearest,
                                      .address = ftk::gpu::Address::clamp});
    nearest_sampler_ = device.sampler({.filter = ftk::gpu::Filter::nearest,
                                       .mip_filter = ftk::gpu::Filter::nearest,
                                       .address = ftk::gpu::Address::clamp});

    if (device.caps().ray_queries) {
        // Set 0 the pass's own, set 1 the bindless material set a hit's
        // surface is read from.
        auto trace = device.create(ftk::gpu::ComputePipelineDesc{
            .shader = "shaders/ss_gi_trace.comp",
            .shared = {resources.bindless},
            .name = "ss_gi_trace",
        });
        if (trace) {
            trace_pipeline_ = std::move(*trace);
            FTK_GFX_INFO("SS-GI trace pipeline created (ray queries)");
        } else {
            FTK_GFX_WARN("Failed to create the SS-GI trace pipeline: {}", trace.error());
        }
    }
    FTK_TRY(resolve_pipeline_, device.create(ftk::gpu::ComputePipelineDesc{
                                     .shader = "shaders/ss_gi_resolve.comp",
                                     .name = "ss_gi_resolve",
                                 }));
    FTK_TRY(upsample_pipeline_, device.create(ftk::gpu::ComputePipelineDesc{
                                      .shader = "shaders/ss_gi_upsample.comp",
                                      .name = "ss_gi_upsample",
                                  }));

    FTK_GFX_DEBUG("SS-GI pass created");
    return {};
}

void SsGiPass::destroy() {
    trace_pipeline_.reset();
    resolve_pipeline_.reset();
    upsample_pipeline_.reset();
}

// ── Frame lifecycle ─────────────────────────────────────────────────────

GpuSsGiConstants SsGiPass::constants(const Images& imgs, const FrameContext& ctx) const {
    GpuSsGiConstants constants{};
    constants.view = ctx.view;
    constants.projection = ctx.projection;

    // The inverse view-projection the depth buffer was drawn with, TAA
    // jitter included; ctx.view/projection may be identity if the
    // FrameContext wasn't fully populated.
    constants.inv_view_proj = ctx.inv_view_proj;

    constants.prev_view_proj = imgs.prev_view_proj;
    constants.prev_inv_view_proj = imgs.prev_inv_view_proj;
    constants.camera_position = ctx.camera_position;
    constants.max_trace_dist = config_.max_trace_dist;
    // Resolution must match what the SS-GI textures were made for (viewport
    // size), not the offscreen render target size (which may be the full window).
    constants.resolution = glm::vec2(imgs.indirect_extent * 2U);
    constants.probe_spacing = config_.probe_spacing;
    constants.frame_index = frame_counter_;
    constants.probe_count = imgs.probe_count;
    constants.trace_cell_size = trace_cell_size_;

    // Sun light for closest-hit direct lighting (cached via set_sun_light)
    constants.sun_direction_intensity = glm::vec4(glm::normalize(-sun_direction_), sun_intensity_);
    constants.sun_color = glm::vec4(sun_color_, 0.0f);
    return constants;
}

void SsGiPass::record_depth_history(const FrameContext& ctx, Images& imgs) {
    if (!imgs.prev_depth || ctx.target == nullptr) return;
    FTK_PROFILE_SCOPE_N("record_ss_gi_depth_history");
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("SS-GI Depth History");
    // The render pipeline fitted the copy to this target's depth; the graph
    // has moved both where a copy needs them.
    cmd.copy(ctx.target->depth_texture(), imgs.prev_depth);
}

void SsGiPass::record(const FrameContext& ctx) {
    if (!active()) { return; }
    auto* imgs = ctx.pp_resources ? ctx.pp_resources->ss_gi() : nullptr;
    if (imgs == nullptr) { return; }
    FTK_PROFILE_SCOPE_N("record_ss_gi");
    // Stopping before the upsample leaves the output holding what it held.
    record_into(ctx, *imgs);
}

PassSetup SsGiPass::setup() const {
    return {
        .name = "ss_gi",
        .phase = RenderPhase::Color,
        .sub_order = 6, // after DDGI (5), which means after depth/normal are ready
    };
}

void SsGiPass::declare(ftk::PassBuilder& builder, const DeclareContext& ctx) {
    if (!active()) { return; }
    // The emissive light the surfaces sample, kept for them into the next
    // frame, which is also what keeps the pass alive. The graph moves it
    // into GENERAL, and back to rest afterwards however record() ends.
    auto out = builder.import_named_optional(ctx, OUTPUT_NAME);
    if (!out.valid()) { return; }
    builder.write(out, ftk::gpu::Access::storage_write_compute);
    // The trace, the resolve and the upsample all read both in compute.
    builder.read(builder.import_named(ctx, "depth"), ftk::gpu::Access::sampled_compute);
    builder.read(builder.import_named(ctx, "normal"), ftk::gpu::Access::sampled_compute);
    // The last frame's depth, for the resolve, before the depth history
    // pass copies this frame's over it.
    if (auto prev = builder.import_named_optional(ctx, PREV_DEPTH_NAME); prev.valid()) {
        builder.read(prev, ftk::gpu::Access::sampled_compute);
    }
    AccelStructureManager::declare_trace(builder, ctx);
}

// ── Record ──────────────────────────────────────────────────────────────

void SsGiPass::record_into(const FrameContext& ctx, Images& imgs) {
    if (!trace_pipeline_ || !imgs.raw_radiance || !imgs.prev_depth) return;
    if (ctx.target == nullptr || ctx.mega_buffer == nullptr) return;
    if (accel_manager_ == nullptr || ctx.scene == nullptr) return;
    // This frame's TLAS for the scene, built by the rt_scene pass; none when
    // no tracer's reach held anything.
    const uint64_t scene_id = ctx.scene->id();
    const ftk::gpu::AccelerationStructure tlas = accel_manager_->tlas(scene_id, ctx.frame_index);
    if (!tlas.valid()) return;

    auto& cmd = ctx.commands();
    auto zone = cmd.zone("SS-GI");
    const ftk::gpu::BufferRange constants = cmd.transient(this->constants(imgs, ctx));
    const ftk::gpu::Texture depth = ctx.target->depth_texture();
    const ftk::gpu::Texture normal = ctx.target->normal_texture();

    // The trace and the resolve each write a whole probe texture, after the
    // last frame's reads of it.
    const std::array<ftk::gpu::TextureView, 2> probe_outputs{imgs.raw_radiance, imgs.resolved};
    cmd.barrier(probe_outputs, REST, ftk::gpu::Access::storage_write_compute);

    {
        FTK_PROFILE_SCOPE_N("ss_gi_trace");
        auto trace_zone = cmd.zone("SS-GI Trace");
        cmd.set_pipeline(trace_pipeline_);
        cmd.bind({{"SsGiConstants", ftk::gpu::uniform(constants)},
                  {"tlas", ftk::gpu::acceleration(tlas)},
                  {"depth_tex", ftk::gpu::sampled(depth, nearest_sampler_)},
                  {"normal_tex", ftk::gpu::sampled(normal, nearest_sampler_)},
                  {"radiance_out", ftk::gpu::storage(imgs.raw_radiance)},
                  {"VertexBuffer", ftk::gpu::storage(ctx.mega_buffer->vertex_buffer())},
                  {"IndexBuffer", ftk::gpu::storage(ctx.mega_buffer->index_buffer())},
                  {"InstanceDataBuffer",
                   ftk::gpu::storage(accel_manager_->instance_data_buffer(scene_id, ctx.frame_index))}});
        cmd.bind(ctx.bindless);
        cmd.push(TracePush{
            .rays_per_probe = imgs.rays_per_probe,
            .ray_offset = frame_counter_ * imgs.rays_per_probe,
        });
        // One invocation per ray: x and y the probe, z the ray.
        cmd.dispatch((imgs.probe_count.x + 7) / 8, (imgs.probe_count.y + 7) / 8, imgs.rays_per_probe);
    }
    cmd.barrier(imgs.raw_radiance, ftk::gpu::Access::storage_write_compute, REST);

    // Temporal accumulation onto the history.
    {
        FTK_PROFILE_SCOPE_N("ss_gi_resolve");
        auto resolve_zone = cmd.zone("SS-GI Resolve");
        cmd.set_pipeline(resolve_pipeline_);
        cmd.bind({{"raw_radiance", ftk::gpu::sampled(imgs.raw_radiance, nearest_sampler_)},
                  {"history", ftk::gpu::sampled(imgs.history, linear_sampler_)},
                  {"depth_tex", ftk::gpu::sampled(depth, nearest_sampler_)},
                  {"normal_tex", ftk::gpu::sampled(normal, nearest_sampler_)},
                  {"prev_depth_tex", ftk::gpu::sampled(imgs.prev_depth, nearest_sampler_)},
                  {"resolved_out", ftk::gpu::storage(imgs.resolved)},
                  {"SsGiConstants", ftk::gpu::uniform(constants)}});
        cmd.push(ResolvePush{
            .probe_count = imgs.probe_count,
            .rays_per_probe = imgs.rays_per_probe,
            .frame_index = frame_counter_,
            .resolution = glm::vec2(imgs.indirect_extent * 2U),
            .probe_spacing = config_.probe_spacing,
            .hysteresis = imgs.history_valid ? config_.hysteresis : 0.0f,
        });
        cmd.dispatch((imgs.probe_count.x + 7) / 8, (imgs.probe_count.y + 7) / 8, 1);
    }

    // The resolved probes become the next frame's history.
    {
        FTK_PROFILE_SCOPE_N("ss_gi_history_copy");
        auto copy_zone = cmd.zone("SS-GI History Copy");
        cmd.barrier(imgs.resolved, ftk::gpu::Access::storage_write_compute, ftk::gpu::Access::copy_src);
        cmd.barrier(imgs.history, REST, ftk::gpu::Access::copy_dst);
        cmd.copy(imgs.resolved, imgs.history);
        cmd.barrier(imgs.history, ftk::gpu::Access::copy_dst, REST);
    }
    cmd.barrier(imgs.resolved, ftk::gpu::Access::copy_src, REST);

    // The resolve has consumed prev_view_proj; the history it just wrote
    // belongs to this frame's camera, so that becomes next frame's reference.
    imgs.prev_view_proj = glm::inverse(ctx.inv_view_proj);
    imgs.prev_inv_view_proj = ctx.inv_view_proj;
    imgs.history_valid = true;

    // The graph has the indirect output in GENERAL.
    {
        FTK_PROFILE_SCOPE_N("ss_gi_upsample");
        auto upsample_zone = cmd.zone("SS-GI Upsample");
        cmd.set_pipeline(upsample_pipeline_);
        cmd.bind({{"resolved_probes", ftk::gpu::sampled(imgs.resolved, linear_sampler_)},
                  {"depth_tex", ftk::gpu::sampled(depth, nearest_sampler_)},
                  {"normal_tex", ftk::gpu::sampled(normal, nearest_sampler_)},
                  {"indirect_out", ftk::gpu::storage(imgs.indirect)}});
        cmd.push(UpsamplePush{
            .output_size = imgs.indirect_extent,
            .probe_count = imgs.probe_count,
            .probe_spacing = config_.probe_spacing,
            .pad0 = 0.0f,
        });
        cmd.dispatch((imgs.indirect_extent.x + 7) / 8, (imgs.indirect_extent.y + 7) / 8, 1);
    }
    imgs.written = true;

    frame_counter_++;
}

// ── SsGiDepthHistoryPass ────────────────────────────────────────────────────

ftk::Result<> SsGiDepthHistoryPass::create(const PassResources& /*resources*/) {
    return {};
}

void SsGiDepthHistoryPass::destroy() {}

void SsGiDepthHistoryPass::post_create(GpuContext& /*gpu*/, PassRegistry& passes) {
    core_ = &passes.get<SsGiPass>("ss_gi");
}

PassSetup SsGiDepthHistoryPass::setup() const {
    return {
        .name = "ss_gi_depth_history",
        .phase = RenderPhase::Color,
        .sub_order = 7,   // after SS-GI (6), which the graph orders it behind
    };
}

void SsGiDepthHistoryPass::declare(ftk::PassBuilder& builder, const DeclareContext& ctx) {
    if (core_ == nullptr || !core_->active()) { return; }
    auto prev = builder.import_named_optional(ctx, SsGiPass::PREV_DEPTH_NAME);
    if (!prev.valid()) { return; }
    builder.read(builder.import_named(ctx, "depth"), ftk::gpu::Access::copy_src);
    // Persistent, which keeps the pass alive: the next frame's SS-GI reads
    // it, and this frame's has read it before the copy lands.
    builder.write(prev, ftk::gpu::Access::copy_dst);
}

void SsGiDepthHistoryPass::record(const FrameContext& ctx) {
    if (core_ == nullptr || !core_->active() || ctx.target == nullptr) { return; }
    auto* imgs = ctx.pp_resources ? ctx.pp_resources->ss_gi() : nullptr;
    if (imgs == nullptr) { return; }
    core_->record_depth_history(ctx, *imgs);
}

} // namespace fjell
