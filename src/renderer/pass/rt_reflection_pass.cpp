#include "renderer/pass/rt_reflection_pass.hpp"

#include "core/log.hpp"
#include "core/profiler.hpp"
#include "gpu/device.hpp"
#include "renderer/accel_structure.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass/ddgi_pass.hpp"
#include "renderer/pass/sky_cubemap_pass.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/pass_registry.hpp"
#include "renderer/post_process_resources.hpp"
#include "renderer/resources/mega_buffer.hpp"
#include "scene/scene.hpp"

#include <array>
#include <cstdlib>

namespace fjell {

namespace {

// Push constant for the trace. Kept small (16 bytes) so we have budget
// elsewhere if needed.
struct ReflectionPush {
    uint32_t frame_index;
    float max_trace_dist;
    float intensity;
    // Size of the coarsest cell in the traced geometry, in metres — the
    // terrain ray mesh's, since it is the one geometry that is traced at a
    // lower resolution than it is shaded. The trace offsets a ray's origin
    // by how far the two representations can disagree over that distance.
    // Zero where no terrain is loaded.
    float trace_cell_size;
};

// The trace's own constants: sun lighting only. Everything else needed at a
// hit (camera, projection inverse, DDGI grid info) lives in DDGI's
// constants, which the trace binds as they are.
struct ReflectionConstants {
    glm::vec4 sun_dir_intensity;
    glm::vec4 sun_color;
};

// Push constant for the denoise compute. The reprojection matrices are too
// large for the guaranteed push range and travel in a uniform block instead.
struct DenoisePush {
    uint32_t width;
    uint32_t height;
    uint32_t frame_index;
    float hysteresis;
    uint32_t debug_mode;
};

// Push constant for the spatial filter.
struct SpatialPush {
    uint32_t width;
    uint32_t height;
    float strength;
    float depth_sigma;
};

// Where the pass's own textures rest between its dispatches and between
// frames: sampled by its compute shaders.
constexpr gpu::Access REST = gpu::Access::sampled_compute;

} // namespace

// ── create ─────────────────────────────────────────────────────────────

Result<> RtReflectionPass::create(const PassResources& resources) {
    gpu::Device& device = *resources.device;
    linear_sampler_ = device.sampler({.filter = gpu::Filter::linear,
                                      .mip_filter = gpu::Filter::nearest,
                                      .address = gpu::Address::clamp});
    nearest_sampler_ = device.sampler({.filter = gpu::Filter::nearest,
                                       .mip_filter = gpu::Filter::nearest,
                                       .address = gpu::Address::clamp});

    // The trace is only built when the device has ray queries. Without them
    // the pass still exists (so .fjgraph parses) but does nothing.
    if (!device.caps().ray_queries) {
        FJELL_GFX_INFO("RT reflections disabled (no ray queries)");
        return {};
    }

    // Set 0 the pass's own, set 1 the bindless material set a hit's surface
    // is read from.
    FJELL_TRY(trace_pipeline_, device.create(gpu::ComputePipelineDesc{
                                   .shader = "shaders/rt_reflection_trace.comp",
                                   .shared = {resources.bindless},
                                   .name = "rt_reflection_trace",
                               }));
    // One stochastic sample per pixel is unusable raw; this accumulates it
    // across frames. See shaders/rt_reflection_denoise.comp.
    FJELL_TRY(denoise_pipeline_, device.create(gpu::ComputePipelineDesc{
                                     .shader = "shaders/rt_reflection_denoise.comp",
                                     .name = "rt_reflection_denoise",
                                 }));
    // Reconciles neighbouring pixels the temporal pass converged
    // independently. See shaders/rt_reflection_spatial.comp.
    FJELL_TRY(spatial_pipeline_, device.create(gpu::ComputePipelineDesc{
                                     .shader = "shaders/rt_reflection_spatial.comp",
                                     .name = "rt_reflection_spatial",
                                 }));

    // Per-viewport textures are owned by PostProcessResources, made once per
    // viewport at the right size.
    FJELL_GFX_INFO("RT reflection pass created");
    return {};
}

void RtReflectionPass::post_create(GpuContext& /*gpu*/, PassRegistry& passes) {
    sky_ = &passes.get<SkyCubemapPass>("sky_cubemap");
}

// ── Images (per-viewport) ──────────────────────────────────────────────

void RtReflectionPass::Images::create(gpu::Device& gpu, uint32_t width, uint32_t height) {
    extent = {width, height};
    history_valid = false;
    prev_depth_slot = 0;

    auto make = [&](gpu::Format format, std::string_view name) -> gpu::Owned<gpu::Texture> {
        auto made = gpu.create(gpu::TextureDesc{
            .format = format,
            .width = width,
            .height = height,
            .use = gpu::TextureUse::storage | gpu::TextureUse::sampled,
            .initial = gpu::Clear{},
            .name = name,
        });
        if (!made) {
            FJELL_GFX_ERROR("{}", made.error());
            return {};
        }
        return std::move(*made);
    };
    // Cleared, the output shows no reflection until the trace first writes it.
    output = make(gpu::Format::rgba16_float, "rt_reflection_output");
    accumulated = make(gpu::Format::rgba16_float, "rt_reflection_accumulated");
    history = make(gpu::Format::rgba16_float, "rt_reflection_history");
    // At the reflection's resolution, which is what the denoise reprojects in.
    for (auto& slot : prev_depth) {
        slot = make(gpu::Format::r32_float, "rt_reflection_prev_depth");
    }
}

void RtReflectionPass::Images::destroy() {
    output.reset();
    accumulated.reset();
    history.reset();
    for (auto& slot : prev_depth) slot.reset();
    extent = {0, 0};
    history_valid = false;
    prev_depth_slot = 0;
}

void RtReflectionPass::Images::resize(gpu::Device& gpu, uint32_t width, uint32_t height) {
    if (output && extent.x == width && extent.y == height) return;
    destroy();
    if (width > 0 && height > 0) create(gpu, width, height);
}

void RtReflectionPass::destroy() {
    trace_pipeline_.reset();
    denoise_pipeline_.reset();
    spatial_pipeline_.reset();
}

PassSetup RtReflectionPass::setup() const {
    return {
        .name = "rt_reflection",
        .phase = RenderPhase::PostProcess,
        // Alongside ssr_trace (sub_order 0); the composite (sub_order 1)
        // declares its read of the output, which orders it after.
        .sub_order = 0,
    };
}

void RtReflectionPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    if (!ray_traced_active()) return;
    auto out = builder.import_named_optional(ctx, OUTPUT_NAME);
    if (!out.valid()) return;
    // Written whole by the trace, and again by the spatial filter once the
    // denoise has read it, all inside the pass; the composite reads it next,
    // which is what keeps the pass alive.
    builder.write(out, gpu::Access::storage_write_compute);

    AccelStructureManager::declare_trace(builder, ctx);
    // The trace, the denoise and the filter read the surface in compute.
    builder.read(builder.import_named(ctx, "depth"), gpu::Access::sampled_compute);
    builder.read(builder.import_named(ctx, "normal"), gpu::Access::sampled_compute);
    builder.read(builder.import_named(ctx, "mr"), gpu::Access::sampled_compute);
    // A reflection ray that misses reads the sky.
    if (auto sky = builder.import_named(ctx, "sky_cubemap"); sky.valid()) {
        builder.read(sky, gpu::Access::sampled_compute);
    }
    // A hit is lit by this viewport's DDGI probe field, after DDGI has
    // updated it.
    for (std::string_view name :
         {DdgiPass::IRRADIANCE_NAME, DdgiPass::VISIBILITY_NAME, DdgiPass::PROBE_OFFSET_NAME}) {
        if (auto atlas = builder.import_named_optional(ctx, name); atlas.valid()) {
            builder.read(atlas, gpu::Access::sampled_compute);
        }
    }
    builder.read(builder.import_named_buffer(ctx, DdgiPass::PROBE_STATUS_NAME),
                 gpu::Access::storage_buffer_read_compute);
}

void RtReflectionPass::record(const FrameContext& ctx) {
    if (!ray_traced_active() || ctx.target == nullptr || ctx.pp_resources == nullptr) return;
    auto* imgs = ctx.pp_resources->rt_reflection();
    if (imgs == nullptr || !imgs->output) return;

    FJELL_PROFILE_SCOPE_N("record_rt_reflection");
    record_into(ctx, *imgs);
}

void RtReflectionPass::record_into(const FrameContext& ctx, Images& imgs) {
    // Where it stops before the trace, the output holds what it held.
    if (accel_manager_ == nullptr || ctx.scene == nullptr || ctx.mega_buffer == nullptr) return;
    if (sky_ == nullptr) return;
    // This frame's TLAS for the scene, built by the rt_scene pass; none when
    // no tracer's reach held anything.
    const uint64_t scene_id = ctx.scene->id();
    const gpu::AccelerationStructure tlas = accel_manager_->tlas(scene_id, ctx.frame_index);
    if (!tlas.valid()) return;
    // A hit is lit by the probe field this viewport's DDGI keeps, made
    // cleared with every probe untraced before DDGI first runs.
    const DdgiPass::Images* ddgi = ctx.pp_resources->ddgi();
    if (ddgi == nullptr || !ddgi->irradiance || !ddgi->constants[ctx.frame_index]) return;

    auto& cmd = ctx.commands();
    auto zone = cmd.zone("RT Reflection");

    const gpu::Texture depth = ctx.target->depth_texture();
    const gpu::Texture normal = ctx.target->normal_texture();
    const gpu::Texture mr = ctx.target->mr_texture();

    // The graph has the output in storage_write.
    cmd.set_pipeline(trace_pipeline_);
    cmd.bind({{"ddgi", gpu::uniform(ddgi->constants[ctx.frame_index])},
              {"tlas", gpu::acceleration(tlas)},
              {"depth_tex", gpu::sampled(depth, nearest_sampler_)},
              {"normal_tex", gpu::sampled(normal, nearest_sampler_)},
              {"mr_tex", gpu::sampled(mr, linear_sampler_)},
              {"sky_cubemap", gpu::sampled(sky_->sky_texture(), sky_->sky_sampler())},
              {"reflection_out", gpu::storage(imgs.output)},
              {"VertexBuffer", gpu::storage(ctx.mega_buffer->vertex_buffer())},
              {"IndexBuffer", gpu::storage(ctx.mega_buffer->index_buffer())},
              {"InstanceDataBuffer",
               gpu::storage(accel_manager_->instance_data_buffer(scene_id, ctx.frame_index))},
              {"ddgi_irradiance_tex", gpu::sampled(ddgi->irradiance, linear_sampler_)},
              {"ddgi_visibility_tex", gpu::sampled(ddgi->visibility, linear_sampler_)},
              {"ddgi_probe_offset_tex", gpu::sampled(ddgi->probe_offset, nearest_sampler_)},
              {"DdgiProbeStatusSSBO", gpu::storage(ddgi->probe_status)},
              {"rc", gpu::uniform(cmd.transient(ReflectionConstants{
                         .sun_dir_intensity = glm::vec4(glm::normalize(-sun_direction_), sun_intensity_),
                         .sun_color = glm::vec4(sun_color_, 0.0F),
                     }))}});
    cmd.bind(ctx.bindless);
    cmd.push(ReflectionPush{
        .frame_index = frame_counter_++,
        .max_trace_dist = max_trace_dist_,
        .intensity = intensity_,
        .trace_cell_size = trace_cell_size_,
    });
    // One invocation per pixel.
    cmd.dispatch((imgs.extent.x + 7) / 8, (imgs.extent.y + 7) / 8, 1);

    // FJELL_DISABLE_RT_DENOISE=1 skips accumulation and hands the composite
    // the raw trace, for A/B against the denoised image.
    static const bool denoise_disabled = [] {
        const char* v = std::getenv("FJELL_DISABLE_RT_DENOISE");
        return v != nullptr && v[0] == '1';
    }();
    if (denoise_disabled) return;

    record_denoise(ctx, imgs);
}

void RtReflectionPass::record_denoise(const FrameContext& ctx, Images& imgs) {
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("RT Reflection Denoise");

    const gpu::Texture depth = ctx.target->depth_texture();
    const gpu::Texture mr = ctx.target->mr_texture();

    // Ping-pong: read what the previous dispatch wrote, write the other slot.
    const uint32_t read_slot = imgs.prev_depth_slot;
    const uint32_t write_slot = 1U - read_slot;

    // The trace's sample becomes the denoise's input; the accumulation and
    // this frame's depth are written whole, after last frame's reads.
    cmd.barrier(imgs.output, gpu::Access::storage_write_compute, REST);
    const std::array<gpu::TextureView, 2> written{imgs.accumulated, imgs.prev_depth[write_slot]};
    cmd.barrier(written, REST, gpu::Access::storage_write_compute);

    // FJELL_RT_DENOISE_DEBUG=1|2|3 replaces the reflection with a diagnostic
    // view: surviving blend, which test rejected history, or how hard the
    // neighbourhood clamp bit. See rt_reflection_denoise.comp.
    static const uint32_t debug_mode = [] {
        const char* v = std::getenv("FJELL_RT_DENOISE_DEBUG");
        return v != nullptr ? static_cast<uint32_t>(std::atoi(v)) : 0U;
    }();

    cmd.set_pipeline(denoise_pipeline_);
    cmd.bind({{"raw_reflection", gpu::sampled(imgs.output, linear_sampler_)},
              {"history", gpu::sampled(imgs.history, linear_sampler_)},
              {"depth_tex", gpu::sampled(depth, nearest_sampler_)},
              {"prev_depth_tex", gpu::sampled(imgs.prev_depth[read_slot], nearest_sampler_)},
              {"mr_tex", gpu::sampled(mr, nearest_sampler_)},
              {"resolved_out", gpu::storage(imgs.accumulated)},
              {"dc", gpu::uniform(cmd.transient(GpuReflectionDenoiseConstants{
                         .projection = ctx.projection,
                         .inv_view_proj = ctx.inv_view_proj,
                         .prev_view_proj = imgs.prev_view_proj,
                         .prev_inv_view_proj = imgs.prev_inv_view_proj,
                         .camera_position = ctx.camera_position,
                         .pad0 = 0.0F,
                     }))},
              {"prev_depth_out", gpu::storage(imgs.prev_depth[write_slot])}});
    cmd.push(DenoisePush{
        .width = imgs.extent.x,
        .height = imgs.extent.y,
        .frame_index = frame_counter_,
        .hysteresis = imgs.history_valid ? hysteresis_ : 0.0F,
        .debug_mode = debug_mode,
    });
    cmd.dispatch((imgs.extent.x + 7) / 8, (imgs.extent.y + 7) / 8, 1);

    // The accumulation becomes the next frame's history.
    cmd.barrier(imgs.accumulated, gpu::Access::storage_write_compute, gpu::Access::copy_src);
    cmd.barrier(imgs.history, REST, gpu::Access::copy_dst);
    cmd.copy(imgs.accumulated, imgs.history);
    cmd.barrier(imgs.history, gpu::Access::copy_dst, REST);
    cmd.barrier(imgs.accumulated, gpu::Access::copy_src, REST);
    cmd.barrier(imgs.prev_depth[write_slot], gpu::Access::storage_write_compute, REST);

    // Next frame reads this frame's depth and writes the other slot.
    imgs.prev_depth_slot = write_slot;

    // The denoise has consumed prev_view_proj; the history it just wrote
    // belongs to this frame's camera, so that becomes next frame's reference.
    imgs.prev_view_proj = glm::inverse(ctx.inv_view_proj);
    imgs.prev_inv_view_proj = ctx.inv_view_proj;
    imgs.history_valid = true;

    record_spatial(ctx, imgs);
}

void RtReflectionPass::record_spatial(const FrameContext& ctx, Images& imgs) {
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("RT Reflection Spatial");

    // Filters the accumulation into the output, which the denoise has read.
    // What the history keeps is the *unfiltered* accumulation, so the
    // neighbourhood clamp keeps measuring the real sample spread rather than
    // one this pass already smoothed. It leaves the output in storage_write,
    // where the graph expects the pass to leave it.
    cmd.barrier(imgs.output, REST, gpu::Access::storage_write_compute);

    cmd.set_pipeline(spatial_pipeline_);
    cmd.bind({{"reflection_in", gpu::sampled(imgs.accumulated, linear_sampler_)},
              {"depth_tex", gpu::sampled(ctx.target->depth_texture(), nearest_sampler_)},
              {"mr_tex", gpu::sampled(ctx.target->mr_texture(), nearest_sampler_)},
              {"filtered_out", gpu::storage(imgs.output)}});
    cmd.push(SpatialPush{
        .width = imgs.extent.x,
        .height = imgs.extent.y,
        .strength = spatial_strength_,
        .depth_sigma = 0.01F,
    });
    cmd.dispatch((imgs.extent.x + 7) / 8, (imgs.extent.y + 7) / 8, 1);
}

} // namespace fjell
