#include "renderer/pass/water_pass.hpp"

#include "renderer/pass/water_fft_pass.hpp"
#include "renderer/pass_registry.hpp"

#include "core/log.hpp"
#include "core/profiler.hpp"
#include "gpu/command_list.hpp"
#include "gpu/device.hpp"
#include "gpu/shader.hpp"
#include "renderer/bindless.hpp"
#include "renderer/components/terrain_component.hpp"
#include "renderer/components/water_component.hpp"
#include "renderer/gpu_context.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass/lit_surface_reads.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/render_stats.hpp"
#include "renderer/pass/volumetric_fog_pass.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/resources/material.hpp"
#include "renderer/resources/terrain_asset.hpp"
#include "renderer/resources/water_gpu.hpp"
#include "renderer/resources/water_surface.hpp"
#include "renderer/uniform_types.hpp"
#include "scene/scene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <vector>

namespace fjell {

namespace {

// The engine's default water surface, drawn when a node has no material.
// Named for the engine rather than for water: a game is very likely to have
// its own shaders/water.fjsl, and a project shader takes precedence over the
// engine's at the same path — which is right, but would leave a water node
// with no material falling back to whatever the game happens to call water.
constexpr const char* FALLBACK_WATER_SHADER = "shaders/water_default.fjsl";

} // namespace

Result<> WaterPass::create(const PassResources& resources) {
    device_ = resources.device;
    resources_ = resources;

    // The prepass: the surface's depth and nothing else. The same task stage
    // as every colour pipeline and a mesh stage that places each vertex
    // through the same code (fjell_water_mesh.glsl) with an invariant
    // position, so the depth is the displaced surface to the bit, but works
    // out none of the slopes, fold or motion the colour draw shades by; no
    // fragment stage, since nothing it would compute is kept. Tests and writes LESS against the seeded scene depth. Seen
    // from both sides, as the colour pipelines are. Set 0 the globals, set 2
    // the water set, which is the pass's own.
    FJELL_TRY(depth_pipeline_, device_->create(gpu::GraphicsPipelineDesc{
                                   .task = "shaders/water.task",
                                   .mesh = "shaders/water_depth.mesh",
                                   .raster = {.cull = gpu::Cull::none},
                                   .depth = {.test = true, .write = true, .compare = gpu::Compare::less},
                                   .depth_format = resources.scene_depth_format,
                                   .samples = resources.samples,
                                   .shared = {resources.globals},
                                   .name = "water_depth",
                               }));

    // The waterline: a compute dispatch over the froxel columns that asks the
    // wave field, for each, whether the near plane's point lies on the other
    // side of the surface from the camera. Its set 2 is the water set the mesh
    // stage reads; set 3 is the mask it writes.
    FJELL_TRY(line_pipeline_, device_->create(gpu::ComputePipelineDesc{
                                  .shader = "shaders/water_line.comp",
                                  .shared = {resources.globals},
                                  .name = "water_line",
                              }));

    FJELL_TRY(no_medium_, device_->create(gpu::TextureDesc{
                              .kind = gpu::TextureKind::tex3d,
                              .format = gpu::Format::rgba16_float,
                              .width = 1,
                              .height = 1,
                              .use = gpu::TextureUse::sampled,
                              .initial = gpu::Clear{0.0f, 0.0f, 0.0f, 1.0f},
                              .name = "water_no_medium",
                          }));
    FJELL_TRY(no_bed_, device_->create(gpu::TextureDesc{
                           .format = gpu::Format::r16_unorm,
                           .width = 1,
                           .height = 1,
                           .use = gpu::TextureUse::sampled,
                           .initial = gpu::Clear{0.0f, 0.0f, 0.0f, 0.0f},
                           .name = "water_no_bed",
                       }));
    return {};
}

void WaterPass::destroy() {
    material_pipelines_.clear();
    depth_pipeline_.reset();
    line_pipeline_.reset();
    no_medium_.reset();
    no_bed_.reset();
    fields_.clear();
    beds_.clear();
}

void WaterPass::post_create(GpuContext& gpu, PassRegistry& passes) {
    pipeline_cache_ = gpu.pipeline_cache();
    bindless_ = &gpu.bindless_textures();
    simulation_ = passes.try_get<WaterFftPass>("water_fft");
    fog_ = passes.try_get<FogCore>("fog_medium");
    if (simulation_ == nullptr) {
        FJELL_CORE_ERROR("WaterPass: no water_fft pass in the render graph. Water samples the "
                         "simulated sea's textures whether or not a field has one, so without "
                         "that pass it cannot draw.");
    }
}

void WaterPass::prepare_frame(const FrameContext& ctx, GpuContext& /*gpu*/) {
    depth_drawn_for_ = nullptr;
    if (ctx.scene == nullptr || pipeline_cache_ == nullptr) return;

    // The levels follow the camera, not any particular body of water, so
    // they are recentered once for the frame regardless of how many infinite
    // seas the scene holds.
    clipmap_.recenter(ctx.camera_position);

    // How far the camera stands above each body of water, negative under
    // it. Asked of the surface over the camera's column, since the trains
    // move the surface sideways and the height at the camera's rest position
    // is not the height above it.
    camera_gap_.clear();
    for (auto [node, wc] : ctx.scene->view<WaterComponent>()) {
        const auto* spatial = node->as_spatial();
        if (spatial == nullptr) continue;
        const glm::vec3 centre{spatial->world_matrix_[3]};
        const glm::vec2 cam_xz(ctx.camera_position.x, ctx.camera_position.z);
        if (wc->extent == WaterComponent::Extent::bounded) {
            const float half = wc->size * 0.5F;
            if (std::abs(cam_xz.x - centre.x) > half || std::abs(cam_xz.y - centre.z) > half) {
                continue;
            }
        }
        const float surface = centre.y
            + water::height_at_iterative(wc->effective_water, cam_xz, ctx.elapsed_time);
        camera_gap_[node->id] = ctx.camera_position.y - surface;
    }

    // The world runs one simulated sea: the first a water node asks for.
    if (simulation_ != nullptr) {
        const FjWaterData::SimulatedSea* running = nullptr;
        std::optional<FftContributor> sea;
        const Node* asked_first = nullptr;
        const Node* asked_otherwise = nullptr;
        for (auto [node, wc] : ctx.scene->view<WaterComponent>()) {
            const auto* fft = water::simulated_sea(wc->effective_water);
            if (fft == nullptr) continue;
            if (!sea.has_value()) {
                sea = *fft;
                asked_first = node;
                // Drawn by whoever derived this field. A field nothing has
                // derived yet has no sea to show, and gets one next frame.
                if (wc->effective_water.simulated.has_value()) {
                    running = &*wc->effective_water.simulated;
                }
            } else if (*fft != *sea && asked_otherwise == nullptr) {
                asked_otherwise = node;
            }
        }
        // A second body of water with a simulated sea of its own is drawn with
        // the first one's: same textures, at the scale they were made for. Said
        // once, when it starts being true, rather than left to be puzzled over.
        if (asked_otherwise != nullptr && !warned_second_sea_) {
            FJELL_CORE_WARN("Water: '{}' asks for a different simulated sea than '{}'. The world "
                            "runs one simulation, so both are drawn with the sea '{}' describes.",
                            asked_otherwise->name, asked_first->name, asked_first->name);
        }
        warned_second_sea_ = asked_otherwise != nullptr;
        simulation_->set_sea(running);
    }

    gather_fields(ctx);
    gather_wakes(ctx);

    // Pipelines are built here rather than in record(): a first sighting of a
    // material or a hot-reloaded shader means building a pipeline, which
    // does not belong inside command recording.
    bool any = false;
    for (auto [node, wc] : ctx.scene->view<WaterComponent>()) {
        any = true;
        if (wc->material != nullptr) {
            (void)ensure_material_pipeline(wc->material->pipeline_id());
        }
    }
    if (!any) return;
    ensure_fallback_material();
    // A hot-reloaded water.fjsl moves the generation of the fallback entry
    // like any other; keep its pipeline current too.
    if (fallback_ready_) {
        (void)ensure_material_pipeline(fallback_pipeline_id_);
    }
}

void WaterPass::ensure_fallback_material() {
    if (fallback_ready_ || pipeline_cache_ == nullptr || bindless_ == nullptr) return;

    const uint32_t pid = pipeline_cache_->register_shader(FALLBACK_WATER_SHADER);
    if (pipeline_cache_->has_compile_error(pid)) {
        FJELL_GFX_ERROR("WaterPass: '{}' failed to compile: {}", FALLBACK_WATER_SHADER,
                        pipeline_cache_->compile_error(pid));
        return;
    }
    const auto& meta = pipeline_cache_->metadata(pid);

    // A material row seeded with the shader's own defaults, as TerrainPass
    // does for a terrain with no .fjmat behind it.
    // Made once: nothing hands a row back, so a row per attempt would
    // fill the table while the pipeline cannot be built.
    if (fallback_material_id_ == NO_ROW) {
        GpuMaterial gpu{};
        if (meta.custom_param_count > 0) {
            gpu.param_offset = bindless_->allocate_params(meta.custom_param_count);
            gpu.param_count = meta.custom_param_count;
            std::vector<float> seed(meta.custom_param_count, 0.0F);
            for (const auto& u : meta.uniforms) {
                if (u.type == UniformType::Sampler2D) continue;
                const uint32_t count = uniform_float_count(u.type);
                for (uint32_t i = 0; i < count && i < u.default_value.size(); ++i) {
                    seed[u.slot + i] = u.default_value[i];
                }
                encode_uniform_for_gpu(u, std::span{seed}.subspan(u.slot, count));
            }
            bindless_->write_params(gpu.param_offset, seed.data(), meta.custom_param_count);
        }
        fallback_material_id_ = bindless_->add_material(gpu);
    }
    fallback_pipeline_id_ = pid;
    fallback_ready_ = ensure_material_pipeline(pid).valid();
}

gpu::GraphicsPipeline WaterPass::ensure_material_pipeline(uint32_t pipeline_id) {
    if (pipeline_cache_ == nullptr || pipeline_id >= pipeline_cache_->shader_count()) {
        return {};
    }
    const uint32_t generation = pipeline_cache_->generation(pipeline_id);
    auto it = material_pipelines_.find(pipeline_id);
    if (it != material_pipelines_.end() && it->second.generation == generation) {
        return it->second.pipeline;
    }

    const auto& meta = pipeline_cache_->metadata(pipeline_id);
    if (meta.shader_type != ShaderType::Water) {
        if (reported_entries_.insert(pipeline_id).second) {
            FJELL_GFX_ERROR("WaterPass: '{}' is shader_type '{}', water needs "
                            "shader_type water",
                            pipeline_cache_->shader_path(pipeline_id), shader_type_name(meta.shader_type));
        }
        return {};
    }
    const auto kept = [&]() -> gpu::GraphicsPipeline {
        return it != material_pipelines_.end() ? gpu::GraphicsPipeline(it->second.pipeline) : gpu::GraphicsPipeline{};
    };
    if (pipeline_cache_->has_compile_error(pipeline_id) || generation == 0) {
        // A failed recompile keeps the pipeline built from the last good one.
        return kept();
    }

    auto fragment = gpu::read_spirv(pipeline_cache_->frag_spv_path(pipeline_id));
    if (!fragment) {
        FJELL_GFX_ERROR("WaterPass: '{}': {}", pipeline_cache_->shader_path(pipeline_id), fragment.error());
        return kept();
    }

    // Water blends over the scene behind it. The three data attachments take
    // the fragment's values straight: a velocity or normal averaged with what
    // the opaque pass left there belongs to neither surface.
    //
    // It tests against the depth the prepass wrote, and writes none: only the
    // nearest surface at each pixel passes, so the sea is hidden by what
    // stands in front of it and hides itself.
    //
    // A Gerstner surface displaces horizontally, and where the trains pinch
    // past their limit it folds through itself: a crest then genuinely stands
    // in front of the water behind it, and without the depth the sheets would
    // composite in submission order, which reads as geometry going missing at
    // the crests. Two bodies of water overlapping on screen resolve the same
    // way: the nearer one shades.
    //
    // The attachment is the water's own — `water_depth`, seeded from the scene
    // depth — and not the depth the rest of the frame samples. That separation
    // is the whole design: a dozen shaders read `depth_texture` meaning "the
    // opaque scene", and a transparent surface does not belong in it.
    //
    // LESS_OR_EQUAL because the prepass wrote this surface's own depth, which
    // LESS would reject; the mesh stage's invariant position makes the two
    // draws agree to the bit.
    //
    // A sea is seen from under it as well as over it, and a wave steep enough
    // to fold shows its own back face. Neither is a reason to drop a triangle.
    const gpu::Blend over{true, gpu::BlendFactor::src_alpha, gpu::BlendFactor::one_minus_src_alpha,
                          gpu::BlendFactor::one, gpu::BlendFactor::one_minus_src_alpha};
    auto made = device_->create(gpu::GraphicsPipelineDesc{
        .task = "shaders/water.task",
        .mesh = "shaders/water.mesh",
        .fragment = std::span<const uint32_t>(*fragment),
        .raster = {.cull = gpu::Cull::none},
        .depth = {.test = true, .write = false, .compare = gpu::Compare::less_equal},
        .color = {{.format = resources_.scene_color_format, .blend = over},
                  {.format = resources_.scene_mr_format},
                  {.format = resources_.scene_normal_format},
                  {.format = resources_.scene_velocity_format}},
        .depth_format = resources_.scene_depth_format,
        .samples = resources_.samples,
        .shared = {resources_.globals, resources_.bindless},
        .name = pipeline_cache_->shader_path(pipeline_id),
    });
    if (!made) {
        FJELL_GFX_ERROR("WaterPass: failed to create pipeline for '{}': {}",
                        pipeline_cache_->shader_path(pipeline_id), made.error());
        return kept();
    }

    // The old pipeline may still be drawn with by a frame in flight; the
    // device releases it once that frame is done.
    auto& slot = material_pipelines_[pipeline_id];
    slot.pipeline = std::move(*made);
    slot.generation = generation;
    return slot.pipeline;
}

void WaterPass::resolve_node_material(const WaterComponent& comp, gpu::GraphicsPipeline& out_pipeline,
                                       uint32_t& out_material_id) {
    out_pipeline = {};
    out_material_id = fallback_material_id_;

    if (comp.material != nullptr) {
        // The pipeline comes from the shared material either way: a script's
        // override clones the parent's params into its own SSBO row, it does
        // not change which shader draws the surface.
        const gpu::GraphicsPipeline p = ensure_material_pipeline(comp.material->pipeline_id());
        if (p.valid()) {
            out_pipeline = p;
            out_material_id = comp.resolved_material_id();
            return;
        }
    }
    // No material, or one that is not usable water: the engine's own shader
    // at its defaults, so a node still draws rather than vanishing.
    if (fallback_ready_) {
        auto it = material_pipelines_.find(fallback_pipeline_id_);
        if (it != material_pipelines_.end()) {
            out_pipeline = it->second.pipeline;
        }
    }
}

void WaterPass::gather_fields(const FrameContext& ctx) {
    fields_.clear();
    beds_.clear();
    for (auto [node, wc] : ctx.scene->view<WaterComponent>()) {
        GpuWaterField field = to_gpu_field(wc->effective_water);

        // The ground is the terrain whose heights the shore holds: the same
        // array, so the texture drawn from is that terrain's. One that has
        // gone since the step that found it leaves the water without ground
        // until the next step says where it is.
        if (field.shore.present != 0) {
            const FjTerrainData* heights = wc->effective_water.shore->terrain.get();
            gpu::Texture bed{};
            for (auto [terrain_node, tc] : ctx.scene->view<TerrainComponent>()) {
                if (tc->terrain != nullptr && tc->terrain->shared_data().get() == heights) {
                    bed = tc->terrain->height_texture();
                    break;
                }
            }
            if (bed.valid()) {
                beds_[wc] = bed;
            } else {
                field.shore.present = 0;
            }
        }

        // A field with a simulated contributor is drawn with the sea the
        // world is running, which is its own unless another body of water
        // asked first. The textures tile at the running sea's patch whoever
        // samples them, so that is the scale the shader is given — not the
        // one this field would have had. None running yet means none to
        // sample.
        if (field.simulated_patch_size > 0.0f) {
            field.simulated_patch_size = simulation_->active() ? simulation_->patch_size() : 0.0f;
        }
        fields_[wc] = field;
    }
}

void WaterPass::record_water_line(const FrameContext& ctx, gpu::Texture mask) {
    if (ctx.scene == nullptr || !line_pipeline_ || simulation_ == nullptr) return;

    // The body of water nearest the camera, if any is within reach of the
    // near plane. One: a camera cannot be at two surfaces at once.
    const Node* nearest = nullptr;
    const WaterComponent* water = nullptr;
    float nearest_gap = LINE_REACH_METRES;
    for (auto [node, wc] : ctx.scene->view<WaterComponent>()) {
        const auto gap = camera_gap_.find(node->id);
        if (gap == camera_gap_.end() || std::abs(gap->second) >= nearest_gap) continue;
        nearest_gap = std::abs(gap->second);
        nearest = node;
        water = wc;
    }

    WaterPushConstants push{};
    // Flat water when none is in reach: the dispatch then writes an empty
    // mask, and binds a field that is defined.
    GpuWaterField line_field{};
    const auto field = water != nullptr ? fields_.find(water) : fields_.end();
    if (field != fields_.end()) {
        line_field = field->second;
        const auto* spatial = nearest->as_spatial();
        push.surface_height = spatial != nullptr ? spatial->world_matrix_[3].y : 0.0F;
        push.simulated_patch_size = field->second.simulated_patch_size;
        if (const auto range = wake_ranges_.find(water); range != wake_ranges_.end()) {
            push.wake_first = range->second.first;
            push.wake_count = range->second.count;
        }
        push.camera_below = camera_below(nearest->id) ? 1U : 0U;
        push.line_active = 1;
    }

    auto& cmd = ctx.commands();
    cmd.set_pipeline(line_pipeline_);
    cmd.bind(ctx.globals);
    // Set 2 is the water set the mesh stage reads, set 3 the mask written.
    cmd.bind(water_set(WaterSet::line, cmd.transient(line_field), {}, cmd.transient(wake_records()), {},
                       bed_of(water)));
    cmd.bind({{"water_line", gpu::storage(mask)}});
    cmd.push(push);
    const gpu::TextureInfo& size = device_->info(mask);
    cmd.dispatch((size.width + 7) / 8, (size.height + 7) / 8, 1);
}

void WaterPass::gather_wakes(const FrameContext& ctx) {
    wake_ranges_.clear();
    wakes_.clear();
    for (auto [node, wc] : ctx.scene->view<WaterComponent>()) {
        WakeRange range{static_cast<uint32_t>(wakes_.size()), 0};
        for (const water::WakeSource& wake : wc->effective_water.wakes) {
            if (wakes_.size() >= WAKE_RECORDS_PER_FRAME) break;
            wakes_.push_back(to_gpu_wake(wake));
            ++range.count;
        }
        wake_ranges_[wc] = range;
    }
}

std::span<const GpuWakeSource> WaterPass::wake_records() const {
    // A buffer is never bound empty; with no wakes a node's range is empty
    // and the one record is never read.
    static const GpuWakeSource none{};
    return wakes_.empty() ? std::span(&none, 1) : std::span(wakes_);
}

gpu::Texture WaterPass::bed_of(const WaterComponent* water) const {
    const auto bed = beds_.find(water);
    return bed != beds_.end() ? bed->second : gpu::Texture{};
}

std::vector<gpu::BindEntry> WaterPass::water_set(WaterSet use, gpu::BufferRange field,
                                                 gpu::BufferRange clipmap, gpu::BufferRange wakes,
                                                 gpu::Texture medium, gpu::Texture bed) const {
    const gpu::Sampler sampler = simulation_->sampler();
    // The bed is read a sample at a time, so the sampler never filters it.
    std::vector<gpu::BindEntry> entries{
        {"water", gpu::uniform(field)},
        {"simulated_displacement", gpu::sampled(simulation_->displacement(), sampler)},
        {"wakes", gpu::storage(wakes)},
        {"bed_heights", gpu::sampled(bed.valid() ? bed : no_bed_.get(), sampler)},
    };
    if (use == WaterSet::line) return entries;
    entries.push_back({"clip", gpu::uniform(clipmap)});
    if (use == WaterSet::depth) return entries;
    entries.push_back({"simulated_previous", gpu::sampled(simulation_->previous_displacement(), sampler)});
    entries.push_back({"simulated_derivatives", gpu::sampled(simulation_->derivatives(), sampler)});
    if (use == WaterSet::colour) {
        entries.push_back({"medium_volume",
                           medium.valid() ? gpu::sampled(medium, fog_->fog_sampler())
                                          : gpu::sampled(no_medium_, sampler)});
    }
    return entries;
}

bool WaterPass::scene_has_water(const FrameContext& ctx) {
    if (ctx.scene == nullptr) {
        return false;
    }
    return !ctx.scene->view<WaterComponent>().empty();
}

bool WaterPass::draws(const FrameContext& ctx) const {
    return ctx.target != nullptr && device_ != nullptr && device_->caps().mesh_shaders &&
           simulation_ != nullptr && scene_has_water(ctx);
}

void WaterPass::seed_water_depth(const FrameContext& ctx) {
    if (!draws(ctx)) return;
    FJELL_PROFILE_SCOPE_N("record_water_depth_seed");
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("Water Depth Seed");
    cmd.copy(ctx.target->msaa_depth_texture(), ctx.target->water_depth_texture());
}

PassSetup WaterPass::setup() const {
    return {
        .name = "water",
        .phase = RenderPhase::Color,
        .sub_order = 45,   // before the transparent pass (50), which carries on over it
    };
}

void WaterPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    if (!ctx.water_draws) { return; }
    // The colour targets the transparent pass then loads, drawn over.
    for (const char* name : {"msaa_color", "msaa_mr", "msaa_normal", "msaa_velocity"}) {
        builder.write(builder.import_named(ctx, name), gpu::Access::color_attachment);
    }
    // The depth the prepass wrote, tested and not written.
    builder.read(builder.import_named(ctx, "msaa_surface_depth"),
                 gpu::Access::depth_attachment_read);
    // What the surface shader samples through the global set: the lighting,
    // the scene depth behind it for WATER_DEPTH, and the copy of the scene
    // it refracts.
    declare_lit_surface_reads(builder, ctx);
    builder.read(builder.import_named(ctx, "depth"), gpu::Access::sampled_fragment);
    builder.read(builder.import_named(ctx, "screen_color"), gpu::Access::sampled_fragment);
    // The simulated sea, which the mesh stage moves the surface by.
    declare_simulation_reads(builder, ctx, gpu::Access::sampled_mesh);
    // The fog integrated front to back, for the medium behind the surface.
    // A transient: read by re-creating it by name, which is also what keeps
    // it from being aliased before this reads it.
    if (ctx.volumetric_fog_enabled) {
        builder.read(builder.create(FogCore::INTEGRATED_NAME, FogCore::volume_desc()),
                     gpu::Access::sampled_fragment);
    }
}

void WaterPass::declare_displacement_read(PassBuilder& builder, const DeclareContext& ctx,
                                          gpu::Access access) {
    if (auto image = builder.import_named_optional(ctx, "water_displacement"); image.valid()) {
        builder.read(image, access);
    }
}

void WaterPass::declare_simulation_reads(PassBuilder& builder, const DeclareContext& ctx,
                                         gpu::Access access) {
    for (const char* name : {"water_displacement", "water_displacement_prev",
                             "water_derivatives"}) {
        if (auto image = builder.import_named_optional(ctx, name); image.valid()) {
            builder.read(image, access);
        }
    }
}

void WaterPass::draw_nodes(const FrameContext& ctx, gpu::RenderEncoder& pass,
                           gpu::GraphicsPipeline depth_only, gpu::Texture medium) {
    // The same clipmap and wakes for every node; each node its own field.
    const gpu::BufferRange clipmap = pass.transient(clipmap_.gpu());
    const gpu::BufferRange wakes = pass.transient(wake_records());
    const WaterSet use = depth_only.valid() ? WaterSet::depth : WaterSet::colour;

    for (auto [node, wc] : ctx.scene->view<WaterComponent>()) {
        const auto field = fields_.find(wc);
        if (field == fields_.end()) continue;

        // A node without a usable colour pipeline is not drawn at all, its
        // depth included, so the surface depth never holds water nobody sees.
        gpu::GraphicsPipeline pipeline{};
        uint32_t material_id = 0;
        resolve_node_material(*wc, pipeline, material_id);
        if (!pipeline.valid()) continue;
        if (depth_only.valid()) pipeline = depth_only;

        pass.set_pipeline(pipeline);
        pass.bind(ctx.globals);
        if (use == WaterSet::colour) pass.bind(ctx.bindless);
        pass.bind(water_set(use, pass.transient(field->second), clipmap, wakes, medium, bed_of(wc)));

        const bool infinite = wc->extent == WaterComponent::Extent::infinite;
        const uint32_t patches = wc->patches_per_side > 0 ? wc->patches_per_side : 1;
        const auto* spatial = node->as_spatial();
        const glm::mat4 world = spatial != nullptr ? spatial->world_matrix_ : glm::mat4{1.0F};
        const glm::vec3 centre = glm::vec3(world[3]);

        WaterPushConstants push{};
        // The node's position is the middle of the surface, which is where a
        // placed thing is expected to be; the shaders generate from a corner.
        // An infinite sea takes only the height from the node — it has no
        // corner and no size, and the clipmap says where its patches are.
        push.world_origin = glm::vec2(centre.x, centre.z) - glm::vec2(wc->size * 0.5F);
        push.world_size = wc->size;
        push.surface_height = centre.y;
        push.patches_per_side = patches;
        push.patch_quads = static_cast<uint32_t>(WATER_PATCH_QUADS);
        push.water_material_id = material_id;
        push.delta_time = ctx.delta_time;
        push.wave_direction = water::dominant_direction(wc->effective_water);
        push.infinite = infinite ? 1U : 0U;
        push.simulated_patch_size = field->second.simulated_patch_size;
        push.sdf_volume_min = sdf_volume_min_;
        push.sdf_volume_max = sdf_volume_max_;
        if (const auto range = wake_ranges_.find(wc); range != wake_ranges_.end()) {
            push.wake_first = range->second.first;
            push.wake_count = range->second.count;
        }
        push.camera_below = camera_below(node->id) ? 1U : 0U;
        // The froxel range to map the medium's samples by, for a water that
        // names a medium, in a run that built the fog volume.
        if (medium.valid() && fog_ != nullptr && wc->medium_material != nullptr) {
            push.medium_near = FogCore::NEAR_PLANE;
            push.medium_far = fog_->far_plane();
            push.medium_slices = FogCore::DEPTH_SLICES;
        }

        pass.push(push);
        pass.draw_mesh_tasks(infinite ? WaterClipmap::patch_count() : patches * patches, 1, 1);

        if (ctx.render_stats != nullptr) {
            ctx.render_stats->draw_calls++;
        }
    }
}

void WaterPass::record_depth(const FrameContext& ctx) {
    if (!draws(ctx) || !depth_pipeline_) return;

    FJELL_PROFILE_SCOPE_N("record_water_depth");
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("Water Depth");
    auto* target = ctx.target;

    // Loaded, because the seed filled it with the scene depth that hides the
    // water behind the opaque scene; stored, because the colour draw tests
    // against it. Under MSAA it also resolves into the surface depth the
    // rest of the frame samples; at one sample it is that image.
    const bool msaa = target->samples() != gpu::Samples::x1;
    auto pass = cmd.render({.depth = gpu::DepthAttachment{
                                .view = target->water_depth_texture(),
                                .resolve = msaa ? gpu::TextureView(target->surface_depth_texture())
                                                : gpu::TextureView{},
                                .resolve_mode = gpu::DepthResolve::sample_zero,
                            }});
    draw_nodes(ctx, pass, depth_pipeline_, {});
    depth_drawn_for_ = target;
}

void WaterPass::record(const FrameContext& ctx) {
    if (!draws(ctx)) return;
    // Without the prepass the attachment holds only the seeded scene, and
    // the water would draw untested against itself; a graph that lists the
    // water without it shows none.
    if (depth_drawn_for_ != ctx.target) {
        if (!warned_no_prepass_) {
            FJELL_GFX_WARN("WaterPass: water_depth_prepass did not draw before the water, so "
                           "the water is skipped. A render graph with the water node needs "
                           "water_depth_seed and water_depth_prepass too.");
            warned_no_prepass_ = true;
        }
        return;
    }
    depth_drawn_for_ = nullptr;

    FJELL_PROFILE_SCOPE_N("record_water");
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("Water");
    auto* target = ctx.target;

    // The sea draws with a depth attachment of its own, because the
    // transparent pass binds the scene depth — a dozen shaders sample that
    // image meaning "the opaque scene", which the water is not part of. Same
    // colour targets, loaded and stored, so the transparent pass that follows
    // carries on over the top of the water.
    //
    // No colour resolve here: the transparent pass resolves the same
    // attachments at its end, and that resolve sees the water's contribution
    // because it resolves the whole image. Resolving twice would be correct
    // and wasted. The depth the prepass left is tested, not written, and
    // kept as it is, since it already is the surface depth.
    auto pass = cmd.render({
        .color = {{.view = target->msaa_color_texture()},
                  {.view = target->msaa_mr_texture()},
                  {.view = target->msaa_normal_texture()},
                  {.view = target->msaa_velocity_texture()}},
        .depth = gpu::DepthAttachment{.view = target->water_depth_texture(),
                                      .access = gpu::Access::depth_attachment_read},
    });
    // The fog integrated front to back, when this run built it.
    draw_nodes(ctx, pass, {}, ctx.texture(FogCore::INTEGRATED_NAME));
}

// ── WaterDepthSeedPass ──────────────────────────────────────────────────────

Result<> WaterDepthSeedPass::create(const PassResources& /*resources*/) {
    return {};
}

void WaterDepthSeedPass::destroy() {}

void WaterDepthSeedPass::post_create(GpuContext& /*gpu*/, PassRegistry& passes) {
    water_ = passes.try_get<WaterPass>("water");
}

PassSetup WaterDepthSeedPass::setup() const {
    return {
        .name = "water_depth_seed",
        .phase = RenderPhase::Color,
        .sub_order = 20,   // once the opaque scene and its overlays are drawn
    };
}

void WaterDepthSeedPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    if (!ctx.water_draws) { return; }
    builder.read(builder.import_named(ctx, "msaa_depth"), gpu::Access::copy_src);
    builder.write(builder.import_named(ctx, "msaa_surface_depth"), gpu::Access::copy_dst);
}

void WaterDepthSeedPass::record(const FrameContext& ctx) {
    if (water_ != nullptr) { water_->seed_water_depth(ctx); }
}

// ── WaterDepthPrepass ───────────────────────────────────────────────────────

Result<> WaterDepthPrepass::create(const PassResources& /*resources*/) {
    return {};
}

void WaterDepthPrepass::destroy() {}

void WaterDepthPrepass::post_create(GpuContext& /*gpu*/, PassRegistry& passes) {
    water_ = passes.try_get<WaterPass>("water");
}

PassSetup WaterDepthPrepass::setup() const {
    return {
        .name = "water_depth_prepass",
        .phase = RenderPhase::Color,
        .sub_order = 21,   // right after the seed
    };
}

void WaterDepthPrepass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    if (!ctx.water_draws) { return; }
    // The seeded attachment, tested and written; under MSAA it resolves into
    // the surface depth as the rendering ends, and without MSAA the two are
    // one image.
    builder.write(builder.import_named(ctx, "msaa_surface_depth"),
                  gpu::Access::depth_attachment);
    builder.write(builder.import_named(ctx, "surface_depth"),
                  ctx.msaa_samples != gpu::Samples::x1 ? gpu::Access::depth_resolve
                                                            : gpu::Access::depth_attachment);
    // The simulated sea's displacement, which the mesh stage places the
    // surface by; its slopes and history are the colour draw's.
    WaterPass::declare_displacement_read(builder, ctx, gpu::Access::sampled_mesh);
}

void WaterDepthPrepass::record(const FrameContext& ctx) {
    if (water_ != nullptr) { water_->record_depth(ctx); }
}

} // namespace fjell
