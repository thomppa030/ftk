#include "renderer/pass/ddgi_pass.hpp"
#include "ftk/gpu/device.hpp"
#include "ftk/gpu/upload.hpp"
#include "ftk/base/profiler.hpp"
#include "renderer/accel_structure.hpp"
#include "ftk/framegraph/pass_builder.hpp"
#include "renderer/pass_registry.hpp"
#include "renderer/post_process_resources.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass/scene_sdf_pass.hpp"
#include "renderer/pass/sky_cubemap_pass.hpp"
#include "renderer/resources/mega_buffer.hpp"
#include "environment/environment_state.hpp"
#include "scene/scene.hpp"
#include "ftk/base/log.hpp"

#include <glm/gtc/random.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>
#include <vector>

namespace fjell {

static_assert(DdgiConfig{}.probe_rays <= DdgiPass::MAX_PROBE_RAYS,
              "the probe update keeps every ray of a probe in shared memory");
static_assert(DdgiConfig{}.irradiance_side_length <= DdgiPass::MAX_ATLAS_SIDE
                  && DdgiConfig{}.visibility_side_length <= DdgiPass::MAX_ATLAS_SIDE,
              "the probe update keeps a probe's whole tile in shared memory");

namespace {

// ddgi_light_select.comp's push constants: the box every probe ray stays
// inside, which a light has to reach to go on the list.
struct LightSelectPushConstants {
    glm::vec3 reach_min;
    float pad0;
    glm::vec3 reach_max;
    float pad1;
};

// The light select runs one invocation per slot of the world light buffer.
constexpr uint32_t LIGHT_SELECT_GROUP = 64;
static_assert(MAX_DDGI_LIGHTS % LIGHT_SELECT_GROUP == 0);

// What a probe's status is until its batch is first traced: the update
// gives it no history and the cage passes over it.
constexpr uint32_t PROBE_STATUS_UNINITIALIZED = 6;

// The radiance texture rests where the update samples it.
constexpr ftk::gpu::Access RADIANCE_REST = ftk::gpu::Access::sampled_compute;

} // namespace

// ── Images ──────────────────────────────────────────────────────────────

static void compute_atlas_dims(const DdgiConfig& config,
                                int32_t& irr_w, int32_t& irr_h,
                                int32_t& vis_w, int32_t& vis_h,
                                int32_t& total) {
    total = config.probe_counts.x * config.probe_counts.y * config.probe_counts.z;
    int probes_per_row = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(total))));

    int irr_side_with_border = config.irradiance_side_length + 2;
    irr_w = probes_per_row * irr_side_with_border;
    irr_h = ((total + probes_per_row - 1) / probes_per_row) * irr_side_with_border;

    int vis_side_with_border = config.visibility_side_length + 2;
    vis_w = probes_per_row * vis_side_with_border;
    vis_h = ((total + probes_per_row - 1) / probes_per_row) * vis_side_with_border;
}

namespace {

ftk::gpu::Owned<ftk::gpu::Texture> make_texture(ftk::gpu::Device& gpu, ftk::gpu::Format format, uint32_t width,
                                      uint32_t height, std::optional<ftk::gpu::Clear> initial,
                                      std::string_view name) {
    auto made = gpu.create(ftk::gpu::TextureDesc{
        .format = format,
        .width = width,
        .height = height,
        .use = ftk::gpu::TextureUse::storage | ftk::gpu::TextureUse::sampled,
        .initial = initial,
        .name = name,
    });
    if (!made) {
        FTK_GFX_ERROR("{}", made.error());
        return {};
    }
    return std::move(*made);
}

ftk::gpu::Owned<ftk::gpu::Buffer> make_buffer(ftk::gpu::Device& gpu, const ftk::gpu::BufferDesc& desc) {
    auto made = gpu.create(desc);
    if (!made) {
        FTK_GFX_ERROR("{}", made.error());
        return {};
    }
    return std::move(*made);
}

// The indirect output, at the screen's size or half of it.
glm::uvec2 output_extent(const DdgiConfig& config, uint32_t screen_w, uint32_t screen_h) {
    if (!config.half_res_output) return {screen_w, screen_h};
    return {std::max(1U, screen_w / 2), std::max(1U, screen_h / 2)};
}

} // namespace

void DdgiPass::Images::create(ftk::gpu::Device& gpu, const DdgiConfig& config,
                              uint32_t screen_w, uint32_t screen_h) {
    compute_atlas_dims(config, irr_width, irr_height, vis_width, vis_height, total_probes);

    // Radiance: one row per probe traced THIS FRAME, not per probe in the
    // pool. The trace writes to `probe_index - probe_update_offset`, so
    // only the round-robin batch is ever addressed. Sizing this by the
    // pool wasted most of the image and put a hard ceiling on probe count:
    // at 32^3 the height would be 32768, twice the 16384 a device is
    // guaranteed to allow, and image creation would simply fail.
    radiance_rows = (config.probes_per_frame_batch > 0)
        ? std::min(config.probes_per_frame_batch, total_probes)
        : total_probes;
    radiance = make_texture(gpu, ftk::gpu::Format::rgba16_float, static_cast<uint32_t>(config.probe_rays),
                            static_cast<uint32_t>(radiance_rows), ftk::gpu::Clear{}, "ddgi_radiance");

    // The probe field starts empty, as a reset leaves it.
    irradiance = make_texture(gpu, ftk::gpu::Format::rgba16_float, static_cast<uint32_t>(irr_width),
                              static_cast<uint32_t>(irr_height), ftk::gpu::Clear{}, "ddgi_irradiance");
    visibility = make_texture(gpu, ftk::gpu::Format::rg16_float, static_cast<uint32_t>(vis_width),
                              static_cast<uint32_t>(vis_height), ftk::gpu::Clear{}, "ddgi_visibility");
    // Probe offsets: total_probes stored as 2D (probes_xy x probes_z)
    const int probes_xy = config.probe_counts.x * config.probe_counts.y;
    probe_offset = make_texture(gpu, ftk::gpu::Format::rgba16_float, static_cast<uint32_t>(probes_xy),
                                static_cast<uint32_t>(config.probe_counts.z), ftk::gpu::Clear{},
                                "ddgi_probe_offset");

    const uint64_t status_size = static_cast<uint64_t>(total_probes) * sizeof(uint32_t);
    probe_status = make_buffer(gpu, {.size = status_size,
                                     .use = ftk::gpu::BufferUse::storage,
                                     .name = "ddgi_probe_status"});
    if (probe_status) {
        gpu.upload().to_buffer(probe_status, 0,
                               std::vector<uint32_t>(static_cast<size_t>(total_probes),
                                                     PROBE_STATUS_UNINITIALIZED));
    }

    for (auto& slot : constants) {
        slot = make_buffer(gpu, {.size = sizeof(GpuDdgiConstants),
                                 .use = ftk::gpu::BufferUse::uniform,
                                 .memory = ftk::gpu::Memory::upload,
                                 .name = "ddgi_constants"});
        // RT reflections read a slot before DDGI has written it when DDGI is
        // off; they find zeros, not whatever the memory held.
        if (slot) std::ranges::fill(gpu.mapped(slot), std::byte{0});
    }
    for (auto& list : light_list) {
        list = make_buffer(gpu, {.size = DDGI_LIGHT_LIST_BYTES,
                                 .use = ftk::gpu::BufferUse::storage,
                                 .name = "ddgi_light_list"});
    }

    indirect_extent = output_extent(config, screen_w, screen_h);
    indirect = make_texture(gpu, ftk::gpu::Format::rgba16_float, indirect_extent.x, indirect_extent.y,
                            std::nullopt, "ddgi_indirect");
    indirect_written = false;

    // Every probe starts over, whatever the atlases these replace held.
    first_frame_ = true;
    probe_update_offset = 0;
}

void DdgiPass::Images::destroy() {
    radiance.reset();
    irradiance.reset();
    visibility.reset();
    probe_offset.reset();
    probe_status.reset();
    for (auto& slot : constants) slot.reset();
    for (auto& list : light_list) list.reset();
    indirect.reset();
    indirect_extent = {0, 0};
    indirect_written = false;
    first_frame_ = true;
}

void DdgiPass::Images::resize(ftk::gpu::Device& gpu, const DdgiConfig& config,
                              uint32_t screen_w, uint32_t screen_h) {
    // Probe count drives the size of every probe atlas, so a change to it
    // means rebuilding all of them. Everything accumulated is discarded —
    // a probe's irradiance belongs to a grid position that no longer
    // exists once the grid is re-divided.
    const int wanted_probes = config.probe_counts.x * config.probe_counts.y * config.probe_counts.z;
    if (!radiance || wanted_probes != total_probes) {
        remake(gpu, config, screen_w, screen_h);
        return;
    }

    // Otherwise only the indirect output follows the screen; the probe
    // field keeps what it holds.
    const glm::uvec2 wanted = output_extent(config, screen_w, screen_h);
    if (wanted == indirect_extent) return;
    indirect_extent = wanted;
    indirect = make_texture(gpu, ftk::gpu::Format::rgba16_float, indirect_extent.x, indirect_extent.y,
                            std::nullopt, "ddgi_indirect");
    indirect_written = false;
}

void DdgiPass::Images::remake(ftk::gpu::Device& gpu, const DdgiConfig& config,
                              uint32_t screen_w, uint32_t screen_h) {
    create(gpu, config, screen_w, screen_h);
}

// ── DdgiPass ────────────────────────────────────────────────────────────

ftk::Result<> DdgiPass::create(const PassResources& resources) {
    ftk::gpu::Device& device = *resources.device;

    // Linear sampler for the SDF, the atlases and the screen
    linear_sampler_ = device.sampler({.filter = ftk::gpu::Filter::linear,
                                      .mip_filter = ftk::gpu::Filter::nearest,
                                      .address = ftk::gpu::Address::clamp});

    FTK_TRY(trace_pipeline_, device.create(ftk::gpu::ComputePipelineDesc{
                                   .shader = "shaders/ddgi_probe_trace_sdf.comp",
                                   .shared = {resources.bindless},
                                   .name = "ddgi_probe_trace_sdf",
                               }));
    FTK_TRY(probe_update_pipeline_, device.create(ftk::gpu::ComputePipelineDesc{
                                          .shader = "shaders/ddgi_probe_update.comp",
                                          .name = "ddgi_probe_update",
                                      }));
    FTK_TRY(sample_pipeline_, device.create(ftk::gpu::ComputePipelineDesc{
                                    .shader = "shaders/ddgi_sample_irradiance.comp",
                                    .name = "ddgi_sample_irradiance",
                                }));
    FTK_TRY(light_select_pipeline_, device.create(ftk::gpu::ComputePipelineDesc{
                                          .shader = "shaders/ddgi_light_select.comp",
                                          .shared = {resources.globals},
                                          .name = "ddgi_light_select",
                                      }));
    FTK_GFX_DEBUG("DDGI pass created (SDF trace + probe update + sample pipelines)");

    // ── RT trace pipeline (optional): compute, tracing with ray queries ──
    if (device.caps().ray_queries) {
        auto rt = device.create(ftk::gpu::ComputePipelineDesc{
            .shader = "shaders/ddgi_probe_trace_rt.comp",
            .shared = {resources.bindless},
            .name = "ddgi_probe_trace_rt",
        });
        if (!rt) {
            FTK_GFX_WARN("Failed to create DDGI RT trace pipeline, falling back to SDF: {}", rt.error());
            backend_ = DdgiBackend::sdf_fallback;
        } else {
            rt_trace_pipeline_ = std::move(*rt);
            backend_ = DdgiBackend::ray_traced;
            FTK_GFX_INFO("DDGI RT trace pipeline created (ray queries)");
        }
    }

    return {};
}

void DdgiPass::destroy() {
    trace_pipeline_.reset();
    probe_update_pipeline_.reset();
    sample_pipeline_.reset();
    light_select_pipeline_.reset();
    rt_trace_pipeline_.reset();
}

void DdgiPass::record(const FrameContext& ctx) {
    if (!config_.enabled) { return; }
    auto* imgs = ctx.pp_resources ? ctx.pp_resources->ddgi() : nullptr;
    if (imgs == nullptr) { return; }
    FTK_PROFILE_SCOPE_N("record_ddgi");
    record_into(ctx, *imgs);
}

GpuDdgiConstants DdgiPass::update_constants(Images& imgs, const FrameContext& ctx) {
    GpuDdgiConstants constants{};
    constants.probe_grid_position = config_.grid_position;
    constants.probe_sphere_scale = 0.1f;
    constants.probe_spacing = config_.probe_spacing;
    constants.max_probe_offset = config_.max_probe_offset;
    constants.reciprocal_spacing = 1.0f / config_.probe_spacing;
    constants.self_shadow_bias = config_.self_shadow_bias;
    constants.probe_counts = config_.probe_counts;
    constants.debug_options = static_cast<uint32_t>(config_.debug_mode);
    constants.irr_tex_w = imgs.irr_width;
    constants.irr_tex_h = imgs.irr_height;
    constants.irr_side = config_.irradiance_side_length;
    constants.probe_rays = config_.probe_rays;
    constants.vis_tex_w = imgs.vis_width;
    constants.vis_tex_h = imgs.vis_height;
    constants.vis_side = config_.visibility_side_length;

    // Round-robin a slice of probes each frame to cut per-frame ray cost.
    //
    // The count can never exceed the radiance texture's height: the trace
    // writes one row per probe at `probe_index - probe_update_offset`, so a
    // wider window writes past the end of the texture and the device is lost.
    // That is a real hazard now the texture is sized to the batch rather than
    // to the whole pool, and it is not only a first-frame concern — a grid
    // re-fit takes the same path.
    int total = imgs.total_probes;
    int max_rows = (imgs.radiance_rows > 0) ? imgs.radiance_rows : total;
    int batch = std::min(config_.probes_per_frame_batch, max_rows);
    bool resetting = imgs.first_frame_ || imgs.reset_generation_ != grid_generation_;
    if (batch <= 0 || batch >= total) {
        constants.probe_update_offset = 0;
        constants.probe_update_count = std::min(total, max_rows);
    } else if (resetting) {
        // A reset cannot refresh the whole pool in one frame any more, so
        // it restarts the sweep instead and lets the round-robin catch up.
        // Every probe is UNINITIALIZED, so each is overwritten rather than
        // blended as the cursor reaches it.
        imgs.probe_update_offset = 0;
        constants.probe_update_offset = 0;
        constants.probe_update_count = batch;
        imgs.probe_update_offset = batch;
    } else {
        if (imgs.probe_update_offset >= total) imgs.probe_update_offset = 0;
        constants.probe_update_offset = imgs.probe_update_offset;
        constants.probe_update_count = std::min(batch, total - imgs.probe_update_offset);
        imgs.probe_update_offset += constants.probe_update_count;
        if (imgs.probe_update_offset >= total) imgs.probe_update_offset = 0;
    }

    constants.hysteresis = config_.hysteresis;
    constants.infinite_bounces_mult = config_.infinite_bounces_mult;
    constants.normal_bias = config_.normal_bias;
    constants.view_bias = config_.view_bias;

    // Random rotation for ray jitter
    static std::mt19937 rng{42};
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);
    float angle = dist(rng) * 2.0f * glm::pi<float>();
    glm::vec3 axis = glm::normalize(glm::vec3(dist(rng) - 0.5f, dist(rng) - 0.5f, dist(rng) - 0.5f));
    constants.random_rotation = glm::rotate(glm::mat4(1.0f), angle, axis);

    constants.camera_position = ctx.camera_position;
    constants.inv_view_proj = ctx.inv_view_proj;
    constants.resolution = glm::vec2(ctx.target ? ctx.target->extent().x : 1920,
                                     ctx.target ? ctx.target->extent().y : 1080);

    const std::span<std::byte> slot = ctx.device->mapped(imgs.constants[ctx.frame_index]);
    std::memcpy(slot.data(), &constants, sizeof(constants));
    return constants;
}

void DdgiPass::record_light_select(const FrameContext& ctx, Images& imgs) {
    FTK_PROFILE_SCOPE_N("ddgi_light_select");
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("DDGI Light Select");
    const ftk::gpu::Buffer list = imgs.light_list[ctx.frame_index];

    // The count starts at zero and the select appends after it. Without the
    // global set there is no light buffer to read, and the list stays empty.
    const ftk::gpu::BufferRange count{list, 0, sizeof(uint32_t)};
    cmd.fill(count, 0);
    cmd.barrier(count, ftk::gpu::Access::clear, ftk::gpu::Access::storage_buffer_read_write_compute);

    if (ctx.globals.valid()) {
        cmd.set_pipeline(light_select_pipeline_);
        cmd.bind(ctx.globals);
        cmd.bind({{"DdgiLightList", ftk::gpu::storage(list)}});
        const AABB reach = trace_reach_bounds();
        cmd.push(LightSelectPushConstants{reach.min, 0.0f, reach.max, 0.0f});
        cmd.dispatch(MAX_DDGI_LIGHTS / LIGHT_SELECT_GROUP, 1, 1);
    }

    // The tracer reads what the select wrote.
    cmd.barrier(ftk::gpu::BufferRange(list), ftk::gpu::Access::storage_buffer_read_write_compute,
                ftk::gpu::Access::storage_buffer_read_compute);
}

void DdgiPass::post_create(GpuContext& /*gpu*/, PassRegistry& passes) {
    scene_sdf_ = passes.try_get<SceneSdfPass>("scene_sdf");
    sky_ = passes.try_get<SkyCubemapPass>("sky_cubemap");
}

void DdgiPass::prepare_frame(const FrameContext& ctx, GpuContext& /*gpu*/) {
    if (ctx.environment != nullptr) {
        const auto& sun = ctx.environment->primary;
        light_direction_ = sun.direction;
        light_intensity_ = sun.intensity;
        light_color_ = sun.color;
        sky_intensity_ = ctx.environment->sky_light_intensity();
    }
}

AABB DdgiPass::trace_reach_bounds() const {
    glm::vec3 grid_min = config_.grid_position;
    glm::vec3 grid_max = config_.grid_position + glm::vec3(config_.probe_counts - glm::ivec3(1)) * config_.probe_spacing;
    return {grid_min - glm::vec3(MAX_TRACE_DIST), grid_max + glm::vec3(MAX_TRACE_DIST)};
}

void DdgiPass::record_into(const FrameContext& ctx, Images& imgs) {
    // What declare() checks before it declares anything: without it the
    // pass does nothing, and the graph has not touched the probe field.
    if (!imgs.irradiance || scene_sdf_ == nullptr || sky_ == nullptr) return;
    const bool use_rt = backend_ == DdgiBackend::ray_traced && rt_trace_pipeline_;
    const ftk::gpu::AccelerationStructure tlas =
        use_rt && accel_manager_ != nullptr && ctx.scene != nullptr
            ? accel_manager_->tlas(ctx.scene->id(), ctx.frame_index)
            : ftk::gpu::AccelerationStructure{};
    if (use_rt && !tlas.valid()) return;

    auto& cmd = ctx.commands();
    auto zone = cmd.zone("DDGI");
    const std::array<ftk::gpu::TextureView, 3> atlases{imgs.irradiance, imgs.visibility, imgs.probe_offset};

    // Past here the probe field is the graph's; a pass that cannot run still
    // leaves it where declare() said it would be.
    if (!imgs.radiance || !imgs.probe_status || !imgs.constants[ctx.frame_index]
        || !imgs.light_list[ctx.frame_index] || (use_rt && ctx.mega_buffer == nullptr)) {
        cmd.barrier(atlases, ftk::gpu::Access::storage_read_write_compute, ATLAS_REST);
        return;
    }

    // The probes integrate the sky; a sky that changes identity (an HDRI
    // finishing its async load a few frames into a scene, or being cleared)
    // leaves every probe lit by the one before, so they start over.
    const ftk::gpu::Texture sky = sky_->sky_texture();
    if (sky != imgs.last_sky) {
        if (imgs.last_sky.valid()) invalidate_probe_lighting();
        imgs.last_sky = sky;
    }

    const GpuDdgiConstants constants = update_constants(imgs, ctx);
    const ftk::gpu::BufferRange constants_buffer = imgs.constants[ctx.frame_index];

    // ── 0. A reset: every probe starts over ──
    // A new grid (a refit, a sky that changed identity) or new textures. The
    // atlases hold probes of another grid, so they are cleared, and every
    // probe is marked untraced: until its batch is traced the update gives
    // it no history and the cage passes over it.
    const bool resetting_atlases = imgs.first_frame_ || imgs.reset_generation_ != grid_generation_;
    if (resetting_atlases) {
        cmd.barrier(ftk::gpu::BufferRange(imgs.probe_status), ftk::gpu::Access::storage_buffer_read_write_compute,
                    ftk::gpu::Access::clear);
        cmd.fill(imgs.probe_status, PROBE_STATUS_UNINITIALIZED);
        cmd.barrier(ftk::gpu::BufferRange(imgs.probe_status), ftk::gpu::Access::clear,
                    ftk::gpu::Access::storage_buffer_read_write_compute);

        cmd.barrier(atlases, ftk::gpu::Access::storage_read_write_compute, ftk::gpu::Access::clear);
        for (const ftk::gpu::TextureView& atlas : atlases) cmd.clear(atlas, ftk::gpu::Clear{});
        // The trace reads the field through samplers.
        cmd.barrier(atlases, ftk::gpu::Access::clear, ftk::gpu::Access::sampled_compute);
    } else {
        cmd.barrier(atlases, ftk::gpu::Access::storage_read_write_compute, ftk::gpu::Access::sampled_compute);
    }

    // ── 1. The trace writes the radiance whole, after last frame's update read it ──
    cmd.barrier(imgs.radiance, RADIANCE_REST, ftk::gpu::Access::storage_write_compute);

    // The lights that reach the grid, for the tracer to pick from at its hits.
    record_light_select(ctx, imgs);

    // ── 2. Trace (RT or SDF) ──
    const ftk::gpu::Buffer lights = imgs.light_list[ctx.frame_index];
    const uint32_t rays = static_cast<uint32_t>(config_.probe_rays);
    const uint32_t probes = static_cast<uint32_t>(constants.probe_update_count);
    if (use_rt) {
        FTK_PROFILE_SCOPE_N("ddgi_trace_rt");
        auto trace_zone = cmd.zone("DDGI Trace RT");
        // The TLAS was built for this scene by the rt_scene pass, before any
        // tracer.
        cmd.set_pipeline(rt_trace_pipeline_);
        cmd.bind({{"ddgi", ftk::gpu::uniform(constants_buffer)},
                  {"tlas", ftk::gpu::acceleration(tlas)},
                  {"radiance_out", ftk::gpu::storage(imgs.radiance)},
                  {"sky_cubemap", ftk::gpu::sampled(sky, sky_->sky_sampler())},
                  {"DdgiProbeStatusSSBO", ftk::gpu::storage(imgs.probe_status)},
                  {"ddgi_probe_offset_tex", ftk::gpu::sampled(imgs.probe_offset, linear_sampler_)},
                  {"VertexBuffer", ftk::gpu::storage(ctx.mega_buffer->vertex_buffer())},
                  {"IndexBuffer", ftk::gpu::storage(ctx.mega_buffer->index_buffer())},
                  {"InstanceDataBuffer",
                   ftk::gpu::storage(accel_manager_->instance_data_buffer(ctx.scene->id(), ctx.frame_index))},
                  {"ddgi_irradiance_tex", ftk::gpu::sampled(imgs.irradiance, linear_sampler_)},
                  {"ddgi_visibility_tex", ftk::gpu::sampled(imgs.visibility, linear_sampler_)},
                  {"scene_sdf", ftk::gpu::sampled(scene_sdf_->volume(), scene_sdf_->sampler())},
                  {"DdgiLightList", ftk::gpu::storage(lights)}});
        cmd.bind(ctx.bindless);
        cmd.push(RtTracePushConstants{
            .light_direction = light_direction_,
            .light_intensity = light_intensity_,
            .light_color = light_color_,
            .max_trace_dist = MAX_TRACE_DIST,
            .sdf_volume_min = sdf_volume_min_,
            .random_seed = frame_counter_,
            .sdf_volume_max = sdf_volume_max_,
            .sky_intensity = sky_intensity_,
            .gi_intensity = config_.gi_intensity,
        });
        // One invocation per ray: x the ray, y the probe in this frame's batch,
        // in workgroups of 32 rays as the SDF trace dispatches.
        cmd.dispatch((rays + 31) / 32, probes, 1);
    } else {
        FTK_PROFILE_SCOPE_N("ddgi_trace_sdf");
        auto trace_zone = cmd.zone("DDGI Trace SDF");
        // SDF fallback trace (only when explicitly configured for SDF)
        cmd.set_pipeline(trace_pipeline_);
        cmd.bind({{"ddgi", ftk::gpu::uniform(constants_buffer)},
                  {"scene_sdf", ftk::gpu::sampled(scene_sdf_->volume(), scene_sdf_->sampler())},
                  {"radiance_out", ftk::gpu::storage(imgs.radiance)},
                  {"sky_cubemap", ftk::gpu::sampled(sky, sky_->sky_sampler())},
                  {"DdgiProbeStatusSSBO", ftk::gpu::storage(imgs.probe_status)},
                  {"ddgi_probe_offset_tex", ftk::gpu::sampled(imgs.probe_offset, linear_sampler_)},
                  {"ddgi_irradiance_tex", ftk::gpu::sampled(imgs.irradiance, linear_sampler_)},
                  {"ddgi_visibility_tex", ftk::gpu::sampled(imgs.visibility, linear_sampler_)},
                  {"DdgiLightList", ftk::gpu::storage(lights)},
                  {"scene_sdf_index", ftk::gpu::sampled(scene_sdf_->index_volume(), scene_sdf_->index_sampler())},
                  {"SdfSlots", ftk::gpu::storage(scene_sdf_->slot_table(ctx.frame_index))}});
        cmd.bind(ctx.bindless);
        cmd.push(TracePushConstants{
            .light_direction = light_direction_,
            .light_intensity = light_intensity_,
            .light_color = light_color_,
            .sdf_resolution = static_cast<float>(SceneSdfPass::DEFAULT_RESOLUTION),
            .sdf_volume_min = sdf_volume_min_,
            .random_seed = frame_counter_,
            .sdf_volume_max = sdf_volume_max_,
            .sky_intensity = sky_intensity_,
            .gi_intensity = config_.gi_intensity,
        });
        cmd.dispatch((rays + 31) / 32, probes, 1);
    }

    // ── 3. The update reads the radiance, and rewrites the field the trace read ──
    cmd.barrier(imgs.radiance, ftk::gpu::Access::storage_write_compute, RADIANCE_REST);
    cmd.barrier(atlases, ftk::gpu::Access::sampled_compute, ftk::gpu::Access::storage_read_write_compute);

    struct UpdatePush {
        float max_trace_dist;
    };

    // ── 4. Probe update: one workgroup per probe traced this frame ──
    // Irradiance, visibility, both tiles' border rings, offset and status,
    // all from the probe's rays read once.
    {
        FTK_PROFILE_SCOPE_N("ddgi_probe_update");
        auto update_zone = cmd.zone("DDGI Probe Update");
        cmd.set_pipeline(probe_update_pipeline_);
        cmd.bind({{"ddgi", ftk::gpu::uniform(constants_buffer)},
                  {"radiance_tex", ftk::gpu::sampled(imgs.radiance, linear_sampler_)},
                  {"irradiance_image", ftk::gpu::storage(imgs.irradiance)},
                  {"visibility_image", ftk::gpu::storage(imgs.visibility)},
                  {"probe_offset_image", ftk::gpu::storage(imgs.probe_offset)},
                  {"ProbeStatusSSBO", ftk::gpu::storage(imgs.probe_status)}});
        cmd.push(UpdatePush{MAX_TRACE_DIST});
        cmd.dispatch(probes, 1, 1);
    }

    // What the update wrote, for the sample and for whoever reads the field
    // after this pass: the field at rest, where declare() says it is left.
    cmd.barrier(atlases, ftk::gpu::Access::storage_read_write_compute, ATLAS_REST);
    cmd.barrier(ftk::gpu::BufferRange(imgs.probe_status), ftk::gpu::Access::storage_buffer_read_write_compute,
                ftk::gpu::Access::storage_buffer_read_compute);

    // ── 5. Screen sampling (skip first frame — depth buffer not yet rendered) ──
    if (!imgs.first_frame_) {
        FTK_PROFILE_SCOPE_N("ddgi_sample");
        auto sample_zone = cmd.zone("DDGI Sample");
        // The graph has the indirect output in GENERAL.
        cmd.set_pipeline(sample_pipeline_);
        cmd.bind({{"ddgi", ftk::gpu::uniform(constants_buffer)},
                  {"depth_tex", ftk::gpu::sampled(ctx.target->depth_texture(), linear_sampler_)},
                  {"normal_tex", ftk::gpu::sampled(ctx.target->normal_texture(), linear_sampler_)},
                  {"ddgi_irradiance_tex", ftk::gpu::sampled(imgs.irradiance, linear_sampler_)},
                  {"ddgi_visibility_tex", ftk::gpu::sampled(imgs.visibility, linear_sampler_)},
                  {"ddgi_probe_offset_tex", ftk::gpu::sampled(imgs.probe_offset, linear_sampler_)},
                  {"DdgiProbeStatusSSBO", ftk::gpu::storage(imgs.probe_status)},
                  {"indirect_out", ftk::gpu::storage(imgs.indirect)}});
        cmd.push(config_.half_res_output ? 1U : 0U);
        cmd.dispatch((imgs.indirect_extent.x + 7) / 8, (imgs.indirect_extent.y + 7) / 8, 1);
        imgs.indirect_written = true;
    } else {
        FTK_GFX_DEBUG("DDGI: First frame dispatched ({} probes, {} rays, indirect {}x{})",
                        imgs.total_probes, config_.probe_rays,
                        imgs.indirect_extent.x, imgs.indirect_extent.y);
    }

    imgs.first_frame_ = false;
    imgs.reset_generation_ = grid_generation_;
    frame_counter_++;
}

PassSetup DdgiPass::setup() const {
    return {
        .name = "ddgi",
        .phase = RenderPhase::Color,
        .sub_order = 5,
    };
}

void DdgiPass::declare(ftk::PassBuilder& builder, const DeclareContext& ctx) {
    if (!config_.enabled || scene_sdf_ == nullptr || sky_ == nullptr) return;
    // This viewport's probe field, where it has one.
    const std::array atlases{builder.import_named_optional(ctx, IRRADIANCE_NAME),
                             builder.import_named_optional(ctx, VISIBILITY_NAME),
                             builder.import_named_optional(ctx, PROBE_OFFSET_NAME)};
    const ftk::FgBuffer status = builder.import_named_buffer(ctx, PROBE_STATUS_NAME);
    if (!std::ranges::all_of(atlases, [](ftk::FgTexture atlas) { return atlas.valid(); }) || !status.valid()) {
        return;
    }
    // The ray-traced tracer traces the scene the rt_scene pass built. With
    // nothing in reach there is no structure, and the pass does nothing: the
    // probes keep what they hold and the output what it held.
    if (backend_ == DdgiBackend::ray_traced && rt_trace_pipeline_
        && !AccelStructureManager::declare_trace(builder, ctx)) {
        return;
    }

    // The trace samples the field, the update rewrites it, and the pass
    // leaves it at rest for the sample and for RT reflections after it.
    for (ftk::FgTexture atlas : atlases) {
        builder.read_write(atlas, ftk::gpu::Access::storage_read_write_compute);
        builder.leaves(atlas, ftk::gpu::Access::storage_read_write_compute, ATLAS_REST);
    }
    builder.read_write(status, ftk::gpu::Access::storage_buffer_read_write_compute);

    // The sample reads the surface.
    builder.read(builder.import_named(ctx, "depth"), ftk::gpu::Access::sampled_compute);
    builder.read(builder.import_named(ctx, "normal"), ftk::gpu::Access::sampled_compute);
    // The SDF tracer marches the scene SDF and names the object at a hit
    // from its index volume; the ray-traced one tests burial in it.
    for (std::string_view name : {SceneSdfPass::VOLUME_NAME, SceneSdfPass::INDEX_NAME}) {
        if (auto sdf = builder.import_named_optional(ctx, name); sdf.valid()) {
            builder.read(sdf, ftk::gpu::Access::sampled_compute);
        }
    }
    // Probe rays that miss read the sky, in either tracer's compute shader.
    if (auto sky = builder.import_named(ctx, "sky_cubemap"); sky.valid()) {
        builder.read(sky, ftk::gpu::Access::sampled_compute);
    }
    // The per-pixel indirect light the surfaces sample. The graph moves it
    // into GENERAL, and back to rest afterwards however record() ends.
    if (auto out = builder.import_named_optional(ctx, OUTPUT_NAME); out.valid()) {
        builder.write(out, ftk::gpu::Access::storage_write_compute);
    }
}

} // namespace fjell
