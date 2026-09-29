// ── Volumetric fog pipeline implementation ──────────────────────────────────
//
// Five RenderPass classes. FogMediumPass IS the FogCore (owns sampler,
// pipelines, Images) and records one dispatch per source of fog; the
// follower nodes hold a FogCore* looked up in post_create() and record one
// dispatch each. Each node declares only the accesses it performs — the
// graph emits one merged barrier per image per node, and fog_medium and
// fog_integrated collapse to [medium..light] and [integrate..composite]
// transients that alias bin-packing can share with other single-frame
// viewport-divisor/16 volumes.
//
// Scatter ping-pong state crosses frame boundaries via the graph's
// per-slice tracker: each frame's final scatter layout (SHADER_READ or
// GENERAL, depending on which index was cur vs prev) carries into the
// next frame, and light/temporal's declared accesses cause the graph
// to emit the right transitions on the first access of the new frame.

#include "renderer/pass/volumetric_fog_pass.hpp"
#include "renderer/pass/water_pass.hpp"
#include "core/profiler.hpp"
#include "renderer/bindless.hpp"
#include "renderer/gpu_context.hpp"
#include "gpu/command_list.hpp"
#include "gpu/device.hpp"
#include "gpu/shader.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/uniform_types.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/pass_registry.hpp"
#include "renderer/post_process_resources.hpp"
#include "renderer/transient_image_pool.hpp"
#include "environment/environment_state.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>

namespace fjell {

namespace {

float halton(int index, int base) {
    float result = 0.0F;
    float f = 1.0F / static_cast<float>(base);
    int i = index;
    while (i > 0) {
        result += f * static_cast<float>(i % base);
        i /= base;
        f /= static_cast<float>(base);
    }
    return result;
}

FogCore::Images* images_for(const FrameContext& ctx, FogCore* core) {
    auto* imgs = ctx.pp_resources != nullptr ? ctx.pp_resources->volumetric_fog() : nullptr;
    return imgs != nullptr ? imgs : (core != nullptr ? &core->images() : nullptr);
}

// The froxel grid for a screen, worked out exactly as the frame graph sizes
// the medium and integrated volumes. The scatter volumes must be that size:
// every dispatch covers their extent, and the composite samples the
// integrated volume across the whole screen, so any froxel column the
// dispatches miss reaches the screen as unwritten memory.
glm::uvec3 froxel_grid(uint32_t screen_w, uint32_t screen_h) {
    return TransientImagePool::resolve_extent(FogCore::volume_desc(), {screen_w, screen_h});
}

// Workgroups covering `size` froxels or pixels, a tile at a time.
uint32_t tiles(uint32_t size) {
    return (size + FogCore::TILE_SIZE - 1) / FogCore::TILE_SIZE;
}

} // namespace

// ── Images ──────────────────────────────────────────────────────────────────

void FogCore::Images::create(gpu::Device& gpu, uint32_t screen_w, uint32_t screen_h) {
    volume_extent = froxel_grid(screen_w, screen_h);
    ping_pong = 0;
    frame_counter = 0;

    for (auto& volume : scatter_volumes) {
        auto made = gpu.create(gpu::TextureDesc{
            .kind = gpu::TextureKind::tex3d,
            .format = VOLUME_FORMAT,
            .width = volume_extent.x,
            .height = volume_extent.y,
            .depth = volume_extent.z,
            .use = gpu::TextureUse::storage | gpu::TextureUse::sampled,
            .name = "fog_scatter",
        });
        if (!made) {
            FJELL_GFX_ERROR("{}", made.error());
            destroy();
            return;
        }
        volume = std::move(*made);
    }
}

void FogCore::Images::destroy() {
    for (auto& volume : scatter_volumes) volume.reset();
    volume_extent = glm::uvec3(0);
}

void FogCore::Images::resize(gpu::Device& gpu, uint32_t screen_w, uint32_t screen_h) {
    const glm::uvec3 grid = froxel_grid(screen_w, screen_h);
    if (grid.x == volume_extent.x && grid.y == volume_extent.y && scatter_volumes[0]) { return; }
    destroy();
    create(gpu, screen_w, screen_h);
}

void FogCore::Images::invalidate_imgui_texture() {
    // No ImGui resources owned by Images — symmetry only, called by
    // PostProcessResources::invalidate_imgui_textures().
}

// ── FogCore ─────────────────────────────────────────────────────────────────

Result<> FogCore::create(Device& /*device*/, const PassResources& resources) {
    gpu_device_ = resources.device;
    globals_ = resources.globals;
    bindless_layout_ = resources.bindless;

    linear_sampler_ = gpu_device_->sampler({.filter = gpu::Filter::linear,
                                            .mip_filter = gpu::Filter::nearest,
                                            .address = gpu::Address::clamp});

    // The medium and light pipelines are built per fog shader in
    // prepare_frame(); these three steps are the engine's own. Temporal and
    // composite read the globals at set 0; integrate binds only its own.
    FJELL_TRY(temporal_pipeline_, gpu_device_->create(gpu::ComputePipelineDesc{
                                      .shader = "shaders/fog_temporal.comp",
                                      .shared = {globals_},
                                      .name = "fog_temporal",
                                  }));
    FJELL_TRY(integrate_pipeline_, gpu_device_->create(gpu::ComputePipelineDesc{
                                       .shader = "shaders/fog_integrate.comp",
                                       .name = "fog_integrate",
                                   }));
    FJELL_TRY(composite_pipeline_, gpu_device_->create(gpu::ComputePipelineDesc{
                                       .shader = "shaders/fog_composite.comp",
                                       .shared = {globals_},
                                       .name = "fog_composite",
                                   }));

    default_images_.create(*gpu_device_, 1, 1);

    FJELL_GFX_DEBUG("Volumetric fog pass created");
    return {};
}

void FogCore::destroy() {
    default_images_.destroy();
    destroy_material_pipelines();
    medium_pipeline_ = {};
    light_pipeline_ = {};
    temporal_pipeline_.reset();
    integrate_pipeline_.reset();
    composite_pipeline_.reset();
}

void FogCore::resize(uint32_t width, uint32_t height) {
    default_images_.resize(*gpu_device_, width, height);
}

void FogCore::post_create(GpuContext& gpu, PassRegistry& /*passes*/) {
    pipeline_cache_ = gpu.pipeline_cache();
    bindless_ = &gpu.bindless_textures();
}

void FogCore::prepare_frame(const FrameContext& ctx, GpuContext& /*gpu*/) {
    enabled_ = false;
    volume_draws_.clear();
    water_draws_.clear();
    if (ctx.environment == nullptr) { return; }

    const auto& fog = ctx.environment->fog;
    if (!fog.enabled) { return; }
    origin_ = fog.origin;
    max_distance_ = fog.max_distance;
    temporal_blend_ = fog.temporal_blend;
    motion_response_ = fog.motion_response;
    // Either the fog node or any volume may ask to see the field; one of them
    // asking is enough, since the field is one thing across the whole scene.
    density_field_debug_ = fog.density_field_debug;
    for (const auto& volume : ctx.environment->fog_volumes) {
        density_field_debug_ = std::max(density_field_debug_, volume.density_field_debug);
    }

    // Pipelines are built here rather than in record(): a first sighting of
    // a material or a hot reload means building pipelines, which does not
    // belong inside command recording. A node with no material, or one
    // whose shader cannot draw fog, gets the engine's own so the scene keeps
    // its fog rather than losing it.
    if (pipeline_cache_ == nullptr || bindless_ == nullptr) { return; }
    ensure_fallback(haze_fallback_, DEFAULT_FOG_SHADER);
    ensure_fallback(volume_fallback_, DEFAULT_FOG_VOLUME_SHADER);
    ensure_fallback(water_fallback_, DEFAULT_WATER_MEDIUM_SHADER);

    uint32_t pipeline_id = fog.pipeline_id;
    uint32_t source_row = fog.material_id;
    const MaterialPipeline* haze = resolve_source(pipeline_id, source_row, haze_fallback_);
    if (haze == nullptr) { return; }
    medium_pipeline_ = haze->medium;
    light_pipeline_ = haze->light;
    apply_instance(haze_instance_, pipeline_id, source_row, fog.uniforms);

    const uint32_t count = std::min(static_cast<uint32_t>(ctx.environment->fog_volumes.size()),
                                    MAX_FOG_VOLUMES);
    for (uint32_t i = 0; i < count; ++i) {
        const FogVolume& volume = ctx.environment->fog_volumes[i];
        uint32_t volume_pipeline = volume.pipeline_id;
        uint32_t volume_row = volume.material_id;
        const MaterialPipeline* mp = resolve_source(volume_pipeline, volume_row, volume_fallback_);
        if (mp == nullptr) { continue; }
        Instance& instance = volume_instances_[volume.node_id];
        apply_instance(instance, volume_pipeline, volume_row, volume.uniforms);
        volume_draws_.push_back({i, mp->medium, instance.material_id, volume.transform});
    }
    for (const WaterMedium& water : ctx.environment->water_media) {
        uint32_t water_pipeline = water.pipeline_id;
        uint32_t water_row = water.material_id;
        const MaterialPipeline* mp = resolve_source(water_pipeline, water_row, water_fallback_);
        if (mp == nullptr) { continue; }
        Instance& instance = water_instances_[water.node_id];
        apply_instance(instance, water_pipeline, water_row, water.uniforms);
        water_draws_.push_back({water.node_id, mp->medium, instance.material_id, water.origin});
    }
    enabled_ = true;
}

void FogCore::ensure_fallback(Fallback& fallback, std::string_view shader) {
    if (fallback.ready || pipeline_cache_ == nullptr || bindless_ == nullptr) { return; }

    const uint32_t pid = pipeline_cache_->register_shader(std::string(shader));
    fallback.pipeline_id = pid;
    if (pipeline_cache_->has_compile_error(pid) || pipeline_cache_->generation(pid) == 0) {
        // Said once: this runs every frame until a hot reload of the file
        // compiles, and the cache has already logged the compiler's output.
        if (reported_entries_.insert(pid).second) {
            FJELL_GFX_ERROR("Fog: '{}' failed to compile; nothing draws with it until it does",
                            shader);
        }
        return;
    }
    reported_entries_.erase(pid);
    const auto& meta = pipeline_cache_->metadata(pid);

    // A material row seeded with the shader's own defaults, as the terrain
    // and water passes keep for a node with no material.
    GpuMaterial gpu{};
    if (meta.custom_param_count > 0) {
        gpu.param_offset = bindless_->allocate_params(meta.custom_param_count);
        gpu.param_count = meta.custom_param_count;
        std::vector<float> seed(meta.custom_param_count, 0.0F);
        for (const auto& u : meta.uniforms) {
            if (u.type == UniformType::Sampler2D) { continue; }
            const uint32_t count = uniform_float_count(u.type);
            for (uint32_t i = 0; i < count && i < u.default_value.size(); ++i) {
                seed[u.slot + i] = u.default_value[i];
            }
            encode_uniform_for_gpu(u, std::span{seed}.subspan(u.slot, count));
        }
        bindless_->write_params(gpu.param_offset, seed.data(), meta.custom_param_count);
    }
    fallback.material_id = bindless_->add_material(gpu);
    fallback.ready = ensure_material_pipeline(pid) != nullptr;
}

const FogCore::MaterialPipeline* FogCore::resolve_source(uint32_t& pipeline_id,
                                                          uint32_t& material_id,
                                                          const Fallback& fallback) {
    if (pipeline_id != FogSettings::NO_MATERIAL) {
        if (const MaterialPipeline* mp = ensure_material_pipeline(pipeline_id)) { return mp; }
    }
    if (!fallback.ready) { return nullptr; }
    pipeline_id = fallback.pipeline_id;
    material_id = fallback.material_id;
    return ensure_material_pipeline(pipeline_id);
}

const FogCore::MaterialPipeline* FogCore::ensure_material_pipeline(uint32_t pipeline_id) {
    if (pipeline_cache_ == nullptr || pipeline_id >= pipeline_cache_->shader_count()) {
        return nullptr;
    }
    const uint32_t generation = pipeline_cache_->generation(pipeline_id);
    auto it = material_pipelines_.find(pipeline_id);
    if (it != material_pipelines_.end() && it->second.generation == generation) {
        return &it->second;
    }

    const auto& meta = pipeline_cache_->metadata(pipeline_id);
    if (meta.shader_type != ShaderType::Fog) {
        if (reported_entries_.insert(pipeline_id).second) {
            FJELL_GFX_ERROR("Fog: '{}' is shader_type '{}', a fog node needs shader_type fog",
                            pipeline_cache_->shader_path(pipeline_id),
                            shader_type_name(meta.shader_type));
        }
        return nullptr;
    }
    if (pipeline_cache_->has_compile_error(pipeline_id) || generation == 0) {
        // A failed recompile keeps the pipelines built from the last good one.
        return it != material_pipelines_.end() ? &it->second : nullptr;
    }

    // Set 0 the globals, set 1 the bindless material set the shader reads its
    // uniforms from, set 2 the step's own images.
    const auto build = [&](const std::string& spv, std::string_view step) -> Result<gpu::Owned<gpu::ComputePipeline>> {
        auto code = gpu::read_spirv(spv);
        if (!code) return std::unexpected(code.error());
        return gpu_device_->create(gpu::ComputePipelineDesc{
            .shader = std::span<const uint32_t>(*code),
            .shared = {globals_, bindless_layout_},
            .name = step,
        });
    };
    auto medium = build(pipeline_cache_->fog_medium_spv(pipeline_id), "fog_medium");
    auto light = build(pipeline_cache_->fog_light_spv(pipeline_id), "fog_light");
    if (!medium || !light) {
        FJELL_GFX_ERROR("Fog: '{}': {}", pipeline_cache_->shader_path(pipeline_id),
                        !medium ? medium.error() : light.error());
        return it != material_pipelines_.end() ? &it->second : nullptr;
    }

    // The old pipelines may still be referenced by a frame in flight; the
    // device releases them once it is done.
    auto& slot = material_pipelines_[pipeline_id];
    slot.medium = std::move(*medium);
    slot.light = std::move(*light);
    slot.generation = generation;
    FJELL_GFX_DEBUG("Fog: built pipelines for '{}'", pipeline_cache_->shader_path(pipeline_id));
    return &slot;
}

void FogCore::apply_instance(Instance& instance, uint32_t pipeline_id,
                             uint32_t source_material_id,
                             const std::vector<NamedValue>& uniforms) {
    const auto& meta = pipeline_cache_->metadata(pipeline_id);
    const uint32_t generation = pipeline_cache_->generation(pipeline_id);
    const GpuMaterial& source = bindless_->material_at(source_material_id);

    // The row follows its source: a new material, a recompiled shader, or
    // textures rebound on the source row all reach the copy. Only the params
    // slice is the pass's own.
    GpuMaterial row = source;
    if (meta.custom_param_count > instance.param_count) {
        instance.param_offset = bindless_->allocate_params(meta.custom_param_count);
        instance.param_count = meta.custom_param_count;
    }
    row.param_offset = instance.param_offset;
    row.param_count = meta.custom_param_count;

    const bool rebuilt = instance.material_id == ~0u
                      || instance.source_material != source_material_id
                      || instance.pipeline_id != pipeline_id
                      || instance.generation != generation;
    if (rebuilt) {
        if (instance.material_id == ~0u) {
            instance.material_id = bindless_->add_material(row);
        } else {
            bindless_->update_material(instance.material_id, row);
        }
        instance.source_material = source_material_id;
        instance.pipeline_id = pipeline_id;
        instance.generation = generation;
        instance.applied = false;
    } else {
        const GpuMaterial& current = bindless_->material_at(instance.material_id);
        if (std::memcmp(&current, &row, sizeof(GpuMaterial)) != 0) {
            bindless_->update_material(instance.material_id, row);
        }
    }

    if (instance.applied && instance.applied_uniforms == uniforms) { return; }
    if (meta.custom_param_count > 0) {
        // The shader's defaults first, so a uniform the resolved list does
        // not name still reads what the .fjsl declares, then every resolved
        // value over its own uniform. A value shorter than the uniform keeps
        // the default in the components it leaves out.
        std::vector<float> values(meta.custom_param_count, 0.0F);
        for (const auto& u : meta.uniforms) {
            if (u.type == UniformType::Sampler2D) { continue; }
            const uint32_t count = uniform_float_count(u.type);
            for (uint32_t i = 0; i < count && i < u.default_value.size(); ++i) {
                values[u.slot + i] = u.default_value[i];
            }
            if (const NamedValue* resolved = find_named(uniforms, u.name)) {
                for (uint32_t i = 0; i < count && i < resolved->count; ++i) {
                    values[u.slot + i] = resolved->values[i];
                }
            }
            encode_uniform_for_gpu(u, std::span{values}.subspan(u.slot, count));
        }
        bindless_->write_params(instance.param_offset, values.data(), meta.custom_param_count);
    }
    instance.applied_uniforms = uniforms;
    instance.applied = true;
}

void FogCore::destroy_material_pipelines() {
    material_pipelines_.clear();
    haze_fallback_.ready = false;
    volume_fallback_.ready = false;
}

FogCore::PushConstants FogCore::make_push(const Images& imgs,
                                           uint32_t width, uint32_t height) const {
    const int frame_idx = static_cast<int>(imgs.frame_counter % 16) + 1;

    PushConstants push{};
    push.volume_dims = glm::uvec4(imgs.volume_extent, imgs.frame_counter);
    push.near_plane = NEAR_PLANE;
    push.far_plane = max_distance_;
    push.temporal_blend = temporal_blend_;
    push.motion_response = motion_response_;
    push.density_field_debug = density_field_debug_;
    push.viewport_width = width;
    push.viewport_height = height;
    push.origin_x = origin_.x;
    push.origin_y = origin_.y;
    push.origin_z = origin_.z;
    push.jitter_x = halton(frame_idx, 3) - 0.5F;
    push.jitter_y = halton(frame_idx, 5) - 0.5F;
    push.jitter_z = halton(frame_idx, 2) - 0.5F;
    push.volume_index = HAZE;
    push.fog_material_id = haze_instance_.material_id;
    push.source_kind = SOURCE_HAZE;
    push.camera_below = 0;
    return push;
}

TextureDesc FogCore::volume_desc() {
    TextureDesc desc{};
    desc.format = VOLUME_FORMAT;
    desc.size_class = SizeClass::viewport;
    desc.viewport_divisor = SCREEN_DIVISOR;
    desc.depth = DEPTH_SLICES;
    desc.kind = gpu::TextureKind::tex3d;
    return desc;
}

// ── FogMediumPass ───────────────────────────────────────────────────────────

namespace {

// The range of workgroups a volume's medium dispatch has to cover: the
// froxels its bounds project to, padded by one on every side for the sample
// jitter and the trilinear reads downstream. Empty when the volume is wholly
// outside the froxel grid.
struct GroupRange {
    glm::uvec3 base{0};
    glm::uvec3 count{0};
    [[nodiscard]] bool empty() const { return count.x == 0 || count.y == 0 || count.z == 0; }
};

GroupRange froxel_groups(const glm::mat4& transform, const glm::mat4& view,
                         const glm::mat4& projection, const FogCore::PushConstants& push) {
    const glm::vec3 grid{push.volume_dims.x, push.volume_dims.y, push.volume_dims.z};
    glm::vec3 lo{std::numeric_limits<float>::max()};
    glm::vec3 hi{std::numeric_limits<float>::lowest()};
    bool crosses_near = false;
    const float log_range = std::log(push.far_plane / push.near_plane);

    for (int i = 0; i < 8; ++i) {
        const glm::vec3 corner{(i & 1) != 0 ? 1.0F : -1.0F,
                               (i & 2) != 0 ? 1.0F : -1.0F,
                               (i & 4) != 0 ? 1.0F : -1.0F};
        const glm::vec4 view_pos = view * transform * glm::vec4(corner, 1.0F);
        const float depth = -view_pos.z;
        // The froxel depth axis is exponential in view depth; a corner in
        // front of the near plane sits at slice zero and one past the far
        // plane past the last.
        const float slice = depth <= push.near_plane
            ? 0.0F
            : std::log(depth / push.near_plane) / log_range * grid.z;
        lo.z = std::min(lo.z, slice);
        hi.z = std::max(hi.z, slice);

        if (depth <= push.near_plane) {
            // A corner at or behind the eye projects nowhere useful; the
            // volume may then cover any column, so all of them are taken.
            crosses_near = true;
            continue;
        }
        const glm::vec4 clip = projection * view_pos;
        const glm::vec2 ndc = glm::vec2(clip) / clip.w;
        const glm::vec2 froxel = (ndc * 0.5F + 0.5F) * glm::vec2(grid);
        lo.x = std::min(lo.x, froxel.x);
        lo.y = std::min(lo.y, froxel.y);
        hi.x = std::max(hi.x, froxel.x);
        hi.y = std::max(hi.y, froxel.y);
    }
    if (crosses_near) {
        lo.x = lo.y = 0.0F;
        hi.x = grid.x;
        hi.y = grid.y;
    }

    // One froxel of padding, then clamp to the grid. A range that ends
    // before it starts is a volume the grid does not see.
    const glm::vec3 pad{1.0F};
    const glm::vec3 first = glm::clamp(glm::floor(lo) - pad, glm::vec3(0.0F), grid);
    const glm::vec3 last = glm::clamp(glm::ceil(hi) + pad, glm::vec3(0.0F), grid);  // exclusive
    GroupRange range;
    if (last.x <= first.x || last.y <= first.y || last.z <= first.z) { return range; }

    const float tile = static_cast<float>(FogCore::TILE_SIZE);
    range.base = glm::uvec3(static_cast<uint32_t>(first.x / tile),
                            static_cast<uint32_t>(first.y / tile),
                            static_cast<uint32_t>(first.z));
    const glm::uvec3 end(static_cast<uint32_t>(std::ceil(last.x / tile)),
                         static_cast<uint32_t>(std::ceil(last.y / tile)),
                         static_cast<uint32_t>(last.z));
    range.count = end - range.base;
    return range;
}

} // namespace

PassSetup FogMediumPass::setup() const {
    return {
        .name = "fog_medium",
        // The volume is built before the water shades, which reads it for the
        // water behind the surface; the surface depth is final by then.
        .phase = RenderPhase::Color,
        .sub_order = 40,
    };
}

void FogMediumPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    if (!enabled()) { return; }

    auto medium = builder.create(MEDIUM_NAME, volume_desc());
    builder.write(medium, gpu::Access::storage_write_compute);
    // The surface depth, for which froxels lie inside a body of water. Read
    // whether or not a water draws this frame, so the set is one shape.
    auto depth = builder.import_named(ctx, COMPOSITE_DEPTH_NAME);
    builder.read(depth, gpu::Access::sampled_compute);
    auto scene_depth = builder.import_named(ctx, "depth");
    builder.read(scene_depth, gpu::Access::sampled_compute);
    // The waterline dispatch recorded at the head of this node binds the
    // water's set, and so the simulated sea's textures.
    WaterPass::declare_simulation_reads(builder, ctx, gpu::Access::sampled_compute);
    // What stirs the fog, through the global set.
    if (auto motion = builder.import_named_optional(ctx, "motion_field"); motion.valid()) {
        builder.read(motion, gpu::Access::sampled_compute);
    }
    if (auto stir = builder.import_named_optional(ctx, "fog_density_field"); stir.valid()) {
        builder.read(stir, gpu::Access::sampled_compute);
    }
    // The waterline mask, written by the water pass's dispatch at the start
    // of this node's recording and read by every medium dispatch after it.
    // One texel per froxel column.
    TextureDesc line_desc{};
    line_desc.format = gpu::Format::r8_unorm;
    line_desc.size_class = SizeClass::viewport;
    line_desc.viewport_divisor = SCREEN_DIVISOR;
    auto line = builder.create(WATER_LINE_NAME, line_desc);
    builder.write(line, gpu::Access::storage_write_compute);
}

void FogMediumPass::record(const FrameContext& ctx) {
    if (!enabled()) { return; }

    auto* imgs = images_for(ctx, this);
    if (imgs == nullptr || !imgs->scatter_volumes[0]) { return; }

    const gpu::Texture medium = ctx.texture(MEDIUM_NAME);
    const gpu::Texture surface_depth = ctx.texture(COMPOSITE_DEPTH_NAME);
    const gpu::Texture scene_depth = ctx.texture("depth");
    const gpu::Texture line = ctx.texture(WATER_LINE_NAME);
    if (!medium.valid() || !surface_depth.valid() || !scene_depth.valid() || !line.valid()) { return; }

    FJELL_PROFILE_SCOPE_N("record_fog_medium");
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("Fog Medium");
    const auto extent = ctx.target->extent();
    const glm::uvec3 grid = imgs->volume_extent;

    // The waterline first, from the water pass, then made visible to the
    // medium dispatches that read it.
    if (auto* water_pass = ctx.passes != nullptr ? ctx.passes->try_get<WaterPass>("water")
                                                 : nullptr) {
        water_pass->record_water_line(ctx, line);
        cmd.barrier(line, gpu::Access::storage_write_compute, gpu::Access::storage_read_compute);
    }

    PushConstants push = make_push(*imgs, extent.width, extent.height);
    const auto bind = [&](gpu::ComputePipeline pipeline) {
        cmd.set_pipeline(pipeline);
        cmd.bind(ctx.globals);
        cmd.bind(ctx.bindless);
        cmd.bind({{"fog_medium", gpu::storage(medium)},
                  {"surface_depth_tex", gpu::sampled(surface_depth, fog_sampler())},
                  {"water_line_img", gpu::storage(line)},
                  {"scene_depth_tex", gpu::sampled(scene_depth, fog_sampler())}});
    };

    // The haze first, over the whole grid: it writes rather than adds, so
    // the image needs no clear and every froxel holds a value before any
    // volume reads it back.
    bind(medium_pipeline());
    cmd.push(push);
    cmd.dispatch(tiles(grid.x), tiles(grid.y), grid.z);

    // Each volume adds over what is there, so every dispatch waits for the
    // one before it.
    gpu::ComputePipeline bound = medium_pipeline();
    const auto next_source = [&](gpu::ComputePipeline pipeline) {
        cmd.barrier(medium, gpu::Access::storage_write_compute, gpu::Access::storage_read_write_compute);
        if (pipeline != bound) {
            bind(pipeline);
            bound = pipeline;
        }
    };
    for (const VolumeDraw& draw : volume_draws()) {
        // Only the froxels the volume's bounds reach; a volume the grid does
        // not see costs nothing.
        const GroupRange range = froxel_groups(draw.transform, ctx.view, ctx.projection, push);
        if (range.empty()) { continue; }

        next_source(draw.pipeline);
        push.volume_index = draw.index;
        push.fog_material_id = draw.material_id;
        push.source_kind = SOURCE_VOLUME;
        push.origin_x = draw.transform[3].x;
        push.origin_y = draw.transform[3].y;
        push.origin_z = draw.transform[3].z;
        push.first_group_x = range.base.x;
        push.first_group_y = range.base.y;
        push.first_group_z = range.base.z;
        cmd.push(push);
        cmd.dispatch(range.count.x, range.count.y, range.count.z);
    }
    push.first_group_x = push.first_group_y = push.first_group_z = 0;

    // Each body of water, over the whole grid: the shader tells which froxels
    // are inside it from the surface depth, on the side the camera is not.
    // The water pass decided that side this frame, from the wave field over
    // the camera's column.
    if (!water_draws().empty() && ctx.passes != nullptr) {
        const auto& water_pass = ctx.passes->get<WaterPass>("water");
        for (const WaterDraw& draw : water_draws()) {
            next_source(draw.pipeline);
            push.volume_index = HAZE;
            push.fog_material_id = draw.material_id;
            push.source_kind = SOURCE_WATER;
            push.camera_below = water_pass.camera_below(draw.node_id) ? 1U : 0U;
            push.origin_x = draw.origin.x;
            push.origin_y = draw.origin.y;
            push.origin_z = draw.origin.z;
            cmd.push(push);
            cmd.dispatch(tiles(grid.x), tiles(grid.y), grid.z);
        }
    }
}

// ── FogLightPass ────────────────────────────────────────────────────────────

Result<> FogLightPass::create(Device& /*device*/, const PassResources& /*resources*/) {
    return {};
}

void FogLightPass::destroy() {}

void FogLightPass::post_create(GpuContext& /*gpu*/, PassRegistry& passes) {
    core_ = &passes.get<FogCore>("fog_medium");
}

PassSetup FogLightPass::setup() const {
    return {
        .name = "fog_light",
        .phase = RenderPhase::Color,
        .sub_order = 41,
    };
}

void FogLightPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    if (core_ == nullptr || !core_->enabled()) { return; }

    auto medium = builder.create(FogCore::MEDIUM_NAME, FogCore::volume_desc());
    builder.read(medium, gpu::Access::storage_read_compute);

    auto scatter_cur = builder.import_named(ctx, "fog_scatter_cur");
    builder.write(scatter_cur, gpu::Access::storage_write_compute);
    builder.has_side_effects();  // writes a persistent cross-frame volume

    // What the light variant samples through the global set.
    if (auto atlas = builder.import_named(ctx, "shadow_atlas"); atlas.valid()) {
        builder.read(atlas, gpu::Access::sampled_compute);
    }
    if (auto atlas = builder.import_named(ctx, "local_shadow_atlas"); atlas.valid()) {
        builder.read(atlas, gpu::Access::sampled_compute);
    }
    if (auto sky = builder.import_named(ctx, "sky_cubemap"); sky.valid()) {
        builder.read(sky, gpu::Access::sampled_compute);
    }
    if (auto clouds = builder.import_named_optional(ctx, "cloud_shadow"); clouds.valid()) {
        builder.read(clouds, gpu::Access::sampled_compute);
    }
    if (auto stir = builder.import_named_optional(ctx, "fog_density_field"); stir.valid()) {
        builder.read(stir, gpu::Access::sampled_compute);
    }
}

void FogLightPass::record(const FrameContext& ctx) {
    if (core_ == nullptr || !core_->enabled()) { return; }

    auto* imgs = images_for(ctx, core_);
    if (imgs == nullptr || !imgs->scatter_volumes[0]) { return; }
    const gpu::Texture medium = ctx.texture(FogCore::MEDIUM_NAME);
    if (!medium.valid()) { return; }

    FJELL_PROFILE_SCOPE_N("record_fog_light");
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("Fog Light");
    const auto extent = ctx.target->extent();
    const glm::uvec3 grid = imgs->volume_extent;

    cmd.set_pipeline(core_->light_pipeline());
    cmd.bind(ctx.globals);
    cmd.bind(ctx.bindless);
    cmd.bind({{"fog_medium", gpu::storage(medium)},
              {"scatter_volume", gpu::storage(imgs->scatter_volumes[imgs->cur_index()])}});
    cmd.push(core_->make_push(*imgs, extent.width, extent.height));
    cmd.dispatch(tiles(grid.x), tiles(grid.y), grid.z);
}

// ── FogTemporalPass ─────────────────────────────────────────────────────────

Result<> FogTemporalPass::create(Device& /*device*/, const PassResources& /*resources*/) {
    return {};
}

void FogTemporalPass::destroy() {}

void FogTemporalPass::post_create(GpuContext& /*gpu*/, PassRegistry& passes) {
    core_ = &passes.get<FogCore>("fog_medium");
}

PassSetup FogTemporalPass::setup() const {
    return {
        .name = "fog_temporal",
        .phase = RenderPhase::Color,
        .sub_order = 42,
    };
}

void FogTemporalPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    if (core_ == nullptr || !core_->enabled()) { return; }

    auto scatter_cur = builder.import_named(ctx, "fog_scatter_cur");
    auto scatter_prev = builder.import_named(ctx, "fog_scatter_prev");
    builder.read(scatter_cur, gpu::Access::storage_read_compute);
    builder.write(scatter_cur, gpu::Access::storage_write_compute);
    builder.read(scatter_prev, gpu::Access::sampled_compute);
    builder.has_side_effects();
}

void FogTemporalPass::record(const FrameContext& ctx) {
    if (core_ == nullptr || !core_->enabled()) { return; }

    auto* imgs = images_for(ctx, core_);
    if (imgs == nullptr || !imgs->scatter_volumes[0]) { return; }

    FJELL_PROFILE_SCOPE_N("record_fog_temporal");
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("Fog Temporal");
    const auto extent = ctx.target->extent();
    const glm::uvec3 grid = imgs->volume_extent;

    cmd.set_pipeline(core_->temporal_pipeline());
    cmd.bind(ctx.globals);
    cmd.bind({{"scatter_volume", gpu::storage(imgs->scatter_volumes[imgs->cur_index()])},
              {"history_volume", gpu::sampled(imgs->scatter_volumes[imgs->prev_index()], core_->fog_sampler())}});
    cmd.push(core_->make_push(*imgs, extent.width, extent.height));
    cmd.dispatch(tiles(grid.x), tiles(grid.y), grid.z);
}

// ── FogIntegratePass ────────────────────────────────────────────────────────

Result<> FogIntegratePass::create(Device& /*device*/, const PassResources& /*resources*/) {
    return {};
}

void FogIntegratePass::destroy() {}

void FogIntegratePass::post_create(GpuContext& /*gpu*/, PassRegistry& passes) {
    core_ = &passes.get<FogCore>("fog_medium");
}

PassSetup FogIntegratePass::setup() const {
    return {
        .name = "fog_integrate",
        .phase = RenderPhase::Color,
        .sub_order = 43,
    };
}

void FogIntegratePass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    if (core_ == nullptr || !core_->enabled()) { return; }

    auto scatter_cur = builder.import_named(ctx, "fog_scatter_cur");
    builder.read(scatter_cur, gpu::Access::sampled_compute);

    auto integrated = builder.create(FogCore::INTEGRATED_NAME, FogCore::volume_desc());
    builder.write(integrated, gpu::Access::storage_write_compute);
}

void FogIntegratePass::record(const FrameContext& ctx) {
    if (core_ == nullptr || !core_->enabled()) { return; }

    auto* imgs = images_for(ctx, core_);
    if (imgs == nullptr || !imgs->scatter_volumes[0]) { return; }
    const gpu::Texture integrated = ctx.texture(FogCore::INTEGRATED_NAME);
    if (!integrated.valid()) { return; }

    FJELL_PROFILE_SCOPE_N("record_fog_integrate");
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("Fog Integrate");
    const auto extent = ctx.target->extent();
    const glm::uvec3 grid = imgs->volume_extent;

    // One invocation per froxel column, marching its slices front to back.
    cmd.set_pipeline(core_->integrate_pipeline());
    cmd.bind({{"scatter_volume", gpu::sampled(imgs->scatter_volumes[imgs->cur_index()], core_->fog_sampler())},
              {"integrated_volume", gpu::storage(integrated)}});
    cmd.push(core_->make_push(*imgs, extent.width, extent.height));
    cmd.dispatch(tiles(grid.x), tiles(grid.y), 1);
}

// ── FogCompositePass ────────────────────────────────────────────────────────

Result<> FogCompositePass::create(Device& /*device*/, const PassResources& /*resources*/) {
    return {};
}

void FogCompositePass::destroy() {}

void FogCompositePass::post_create(GpuContext& /*gpu*/, PassRegistry& passes) {
    core_ = &passes.get<FogCore>("fog_medium");
}

PassSetup FogCompositePass::setup() const {
    return {
        .name = "fog_composite",
        // Stays after the transparents: it fogs the resolved colour for the
        // distance to what is seen, and a transparent drawn after it would
        // go over that fog unfogged.
        .phase = RenderPhase::PostProcess,
        .sub_order = 104,
    };
}

void FogCompositePass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    if (core_ == nullptr || !core_->enabled()) { return; }

    auto integrated = builder.create(FogCore::INTEGRATED_NAME, FogCore::volume_desc());
    builder.read(integrated, gpu::Access::sampled_compute);

    auto depth = builder.import_named(ctx, FogCore::COMPOSITE_DEPTH_NAME);
    auto color = builder.import_named(ctx, "color");
    builder.read(depth, gpu::Access::sampled_compute);
    builder.read_write(color, gpu::Access::storage_read_write_compute);
    // Increment the frame counter once per frame after the last node that
    // uses it. Composite is always last in the fog chain, so flip here.
    // (We mutate through core_ in record(), not declare — here we just
    // mark the access topology.)
}

void FogCompositePass::record(const FrameContext& ctx) {
    if (core_ == nullptr || !core_->enabled()) { return; }

    auto* imgs = images_for(ctx, core_);
    if (imgs == nullptr || !imgs->scatter_volumes[0]) { return; }
    const gpu::Texture integrated = ctx.texture(FogCore::INTEGRATED_NAME);
    const gpu::Texture composite_depth = ctx.texture(FogCore::COMPOSITE_DEPTH_NAME);
    if (!integrated.valid() || !composite_depth.valid()) { return; }

    FJELL_PROFILE_SCOPE_N("record_fog_composite");
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("Fog Composite");
    const auto extent = ctx.target->extent();

    cmd.set_pipeline(core_->composite_pipeline());
    cmd.bind(ctx.globals);
    cmd.bind({{"integrated_volume", gpu::sampled(integrated, core_->fog_sampler())},
              {"depth_texture", gpu::sampled(composite_depth, core_->fog_sampler())},
              {"scene_color", gpu::storage(ctx.target->color_texture())}});
    cmd.push(core_->make_push(*imgs, extent.width, extent.height));
    cmd.dispatch(tiles(extent.width), tiles(extent.height), 1);

    // Advance ping-pong + frame counter after composite (the last node in
    // the fog chain). Next frame's light step will write the new cur_index().
    imgs->ping_pong = imgs->prev_index();
    ++imgs->frame_counter;
}

} // namespace fjell
