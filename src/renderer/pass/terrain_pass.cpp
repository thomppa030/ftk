#include "renderer/pass/terrain_pass.hpp"

#include "renderer/pass/shadow_pass.hpp"

#include "core/log.hpp"
#include "core/profiler.hpp"
#include "gpu/binding.hpp"
#include "gpu/command_list.hpp"
#include "gpu/device.hpp"
#include "gpu/shader.hpp"
#include "renderer/bindless.hpp"
#include "renderer/components/terrain_component.hpp"
#include "renderer/gpu_context.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/render_stats.hpp"
#include "renderer/resources/material.hpp"
#include "renderer/resources/terrain_asset.hpp"
#include "scene/node.hpp"
#include "scene/scene.hpp"

#include <span>
#include <vector>

namespace fjell {

namespace {

// The engine's default terrain surface, drawn when a node has no material.
constexpr const char* FALLBACK_TERRAIN_SHADER = "shaders/terrain.fjsl";

// Terrain is a heightfield seen from above: only its upward faces are ever
// drawn, whichever view it is drawn for.
constexpr gpu::Raster SURFACE_RASTER{.cull = gpu::Cull::back,
                                     .front_face = gpu::FrontFace::counter_clockwise};

// Depth is already written by the prepass, which draws this same surface
// from the same height texture. Testing LESS against our own prepass depth
// rejects every fragment, so the colour draws test equal-or-nearer and do not
// write — the same arrangement every other opaque pipeline uses.
constexpr gpu::Depth COLOR_DEPTH{.test = true, .write = false,
                                 .compare = gpu::Compare::less_equal};

} // namespace

Result<> TerrainPass::create(Device& /*device*/, const PassResources& resources) {
    device_ = resources.device;
    resources_ = resources;

    // Linear filtering so heights interpolate between samples the same way the
    // CPU query does; clamp to edge so border patches read a defined height
    // instead of wrapping to the far side of the terrain. The weight texture
    // shares it: layer transitions should blend, not step. The patch bounds
    // are fetched by texel, so the filter does not reach them.
    sampler_ = device_->sampler({.filter = gpu::Filter::linear,
                                 .mip_filter = gpu::Filter::nearest,
                                 .address = gpu::Address::clamp});

    // Every pipeline shares the task stage, which drops the patches outside
    // the view, and the push block. Set 0 is the globals and set 2 the
    // terrain's own textures, bound by name per node; the colour pipelines
    // add the material set, from which the FJSL fragment reads its layers and
    // its own uniforms.
    //
    // Depth for the prepass: a mesh stage that emits position alone, and an
    // empty fragment stage like the other depth-only pipelines.
    FJELL_TRY(depth_pipeline_, device_->create(gpu::GraphicsPipelineDesc{
                                   .task = "shaders/terrain.task",
                                   .mesh = "shaders/terrain_depth.mesh",
                                   .fragment = "shaders/depth_only.frag",
                                   .raster = SURFACE_RASTER,
                                   .depth = {.test = true, .write = true, .compare = gpu::Compare::less},
                                   .depth_format = resources.scene_depth_format,
                                   .samples = resources.samples,
                                   .shared = {resources.globals},
                                   .name = "terrain_depth",
                               }));

    // The depth pipeline again, into the cascade's own format and with the
    // slope-scaled bias the shadow pass uses for everything else. Terrain is a
    // wide, near-flat receiver seen at a grazing angle from a low sun, which
    // is where an unbiased depth-only draw acnes worst, so the two must agree
    // — a cascade half-written with a different bias reads as bands across
    // the ground. Depth is clamped rather than clipped, as for the other
    // casters, so ground between the cascade and the sun still lands in the
    // map; the task stage keeps it for shadow views. The cascades are
    // single-sampled whatever the camera target is doing.
    gpu::Raster shadow_raster = SURFACE_RASTER;
    shadow_raster.depth_clamp = true;
    shadow_raster.depth_bias = ShadowPass::DEPTH_BIAS_CONSTANT;
    shadow_raster.depth_bias_slope = ShadowPass::DEPTH_BIAS_SLOPE;
    FJELL_TRY(shadow_pipeline_, device_->create(gpu::GraphicsPipelineDesc{
                                    .task = "shaders/terrain.task",
                                    .mesh = "shaders/terrain_depth.mesh",
                                    .fragment = "shaders/depth_only.frag",
                                    .raster = shadow_raster,
                                    .depth = {.test = true, .write = true, .compare = gpu::Compare::less},
                                    .depth_format = ShadowPass::DEPTH_FORMAT,
                                    .shared = {resources.globals},
                                    .name = "terrain_shadow",
                                }));

    // Patch grid overlay: the colour mesh stage, and a flat-colour fragment
    // stage so the lines read against the surface instead of vanishing into
    // its own shading. No culling, so both sides of a ridge show. Colour
    // only: GTAO, SSR and TAA read the other targets, and an editor overlay
    // must not disturb what the fill pass wrote into them.
    FJELL_TRY(wireframe_pipeline_, device_->create(gpu::GraphicsPipelineDesc{
                                       .task = "shaders/terrain.task",
                                       .mesh = "shaders/terrain.mesh",
                                       .fragment = "shaders/terrain_wire.frag",
                                       .raster = {.cull = gpu::Cull::none, .fill = gpu::Fill::lines},
                                       .depth = COLOR_DEPTH,
                                       .color = {{.format = resources.scene_color_format},
                                                 {.format = resources.scene_mr_format, .write = {}},
                                                 {.format = resources.scene_normal_format, .write = {}},
                                                 {.format = resources.scene_velocity_format, .write = {}}},
                                       .depth_format = resources.scene_depth_format,
                                       .samples = resources.samples,
                                       .shared = {resources.globals},
                                       .name = "terrain_wireframe",
                                   }));
    return {};
}

void TerrainPass::destroy() {
    material_pipelines_.clear();
    depth_pipeline_.reset();
    shadow_pipeline_.reset();
    wireframe_pipeline_.reset();
}

void TerrainPass::post_create(GpuContext& gpu, PassRegistry& /*passes*/) {
    pipeline_cache_ = gpu.pipeline_cache();
    bindless_ = &gpu.bindless_textures();
}

void TerrainPass::prepare_frame(const FrameContext& ctx, GpuContext& /*gpu*/) {
    if (ctx.scene == nullptr || pipeline_cache_ == nullptr) return;
    // Pipelines are built here rather than while drawing: a first sighting of
    // a material or a hot-reloaded shader means building a pipeline, which
    // does not belong inside command recording.
    bool any = false;
    for (auto [node, tc] : ctx.scene->view<TerrainComponent>()) {
        if (tc->terrain == nullptr) continue;
        any = true;
        if (tc->material != nullptr) {
            (void)ensure_material_pipeline(tc->material->pipeline_id());
        }
        // Sculpting invalidates the patch bounds the task shader culls
        // against. Catching them up here rather than in apply_heights() means
        // a stroke of many dabs costs one rebuild per frame, and the copy is
        // recorded outside the render pass like every other upload.
        tc->terrain->refresh_patch_bounds(device_->upload());
    }
    if (!any) return;
    ensure_fallback_material();
    // A hot-reloaded terrain.fjsl moves the generation of the fallback entry
    // like any other; keep its pipeline current too.
    if (fallback_ready_) {
        (void)ensure_material_pipeline(fallback_pipeline_id_);
    }
}

void TerrainPass::ensure_fallback_material() {
    if (fallback_ready_ || pipeline_cache_ == nullptr || bindless_ == nullptr) return;

    const uint32_t pid = pipeline_cache_->register_shader(FALLBACK_TERRAIN_SHADER);
    if (pipeline_cache_->has_compile_error(pid)) {
        FJELL_GFX_ERROR("TerrainPass: '{}' failed to compile: {}", FALLBACK_TERRAIN_SHADER,
                        pipeline_cache_->compile_error(pid));
        return;
    }
    const auto& meta = pipeline_cache_->metadata(pid);

    // A material row seeded with the shader's own defaults, as SkyPass does
    // for a sky shader with no .fjmat behind it.
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

gpu::GraphicsPipeline TerrainPass::ensure_material_pipeline(uint32_t pipeline_id) {
    if (pipeline_cache_ == nullptr || pipeline_id >= pipeline_cache_->shader_count()) {
        return {};
    }
    const uint32_t generation = pipeline_cache_->generation(pipeline_id);
    auto it = material_pipelines_.find(pipeline_id);
    if (it != material_pipelines_.end() && it->second.generation == generation) {
        return it->second.pipeline;
    }
    // A failed rebuild keeps the pipeline built from the last good shader.
    auto kept = [&]() -> gpu::GraphicsPipeline {
        return it != material_pipelines_.end() ? gpu::GraphicsPipeline(it->second.pipeline)
                                               : gpu::GraphicsPipeline{};
    };

    const auto& meta = pipeline_cache_->metadata(pipeline_id);
    if (meta.shader_type != ShaderType::Terrain) {
        if (reported_entries_.insert(pipeline_id).second) {
            FJELL_GFX_ERROR("TerrainPass: '{}' is shader_type '{}', a terrain needs "
                            "shader_type terrain",
                            pipeline_cache_->shader_path(pipeline_id), shader_type_name(meta.shader_type));
        }
        return {};
    }
    if (pipeline_cache_->has_compile_error(pipeline_id) || generation == 0) {
        return kept();
    }

    auto fragment = gpu::read_spirv(pipeline_cache_->frag_spv_path(pipeline_id));
    if (!fragment) {
        FJELL_GFX_ERROR("TerrainPass: '{}': {}", pipeline_cache_->shader_path(pipeline_id), fragment.error());
        return kept();
    }

    auto made = device_->create(gpu::GraphicsPipelineDesc{
        .task = "shaders/terrain.task",
        .mesh = "shaders/terrain.mesh",
        .fragment = std::span<const uint32_t>(*fragment),
        .raster = SURFACE_RASTER,
        .depth = COLOR_DEPTH,
        .color = {{.format = resources_.scene_color_format},
                  {.format = resources_.scene_mr_format},
                  {.format = resources_.scene_normal_format},
                  {.format = resources_.scene_velocity_format}},
        .depth_format = resources_.scene_depth_format,
        .samples = resources_.samples,
        .shared = {resources_.globals, resources_.bindless},
        .name = pipeline_cache_->shader_path(pipeline_id),
    });
    if (!made) {
        FJELL_GFX_ERROR("TerrainPass: failed to create pipeline for '{}': {}",
                        pipeline_cache_->shader_path(pipeline_id), made.error());
        return kept();
    }

    // The old pipeline may still be drawn with by a frame in flight; the
    // device releases it once that frame is done.
    auto& slot = material_pipelines_[pipeline_id];
    slot.pipeline = std::move(*made);
    slot.generation = generation;
    FJELL_GFX_DEBUG("TerrainPass: built pipeline for '{}'",
                    pipeline_cache_->shader_path(pipeline_id));
    return slot.pipeline;
}

void TerrainPass::resolve_node_material(const TerrainComponent& comp,
                                        gpu::GraphicsPipeline& out_pipeline,
                                        uint32_t& out_material_id) {
    out_pipeline = {};
    out_material_id = fallback_material_id_;
    if (comp.material != nullptr) {
        auto it = material_pipelines_.find(comp.material->pipeline_id());
        if (it != material_pipelines_.end() && it->second.pipeline) {
            out_pipeline = it->second.pipeline;
            out_material_id = comp.material->material_id();
            return;
        }
    }
    if (fallback_ready_) {
        if (auto it = material_pipelines_.find(fallback_pipeline_id_); it != material_pipelines_.end()) {
            out_pipeline = it->second.pipeline;
        }
    }
}

bool TerrainPass::scene_has_terrain(const FrameContext& ctx) {
    if (ctx.scene == nullptr) {
        return false;
    }
    for (auto [node, tc] : ctx.scene->view<TerrainComponent>()) {
        if (tc->terrain != nullptr) {
            return true;
        }
    }
    return false;
}

void TerrainPass::draw_terrain_nodes(gpu::RenderEncoder& pass, const FrameContext& ctx,
                                     Variant variant, uint32_t view_index, glm::uvec2 extent) {
    gpu::GraphicsPipeline bound{};

    for (auto [node, terrain_comp] : ctx.scene->view<TerrainComponent>()) {
        if (terrain_comp->terrain == nullptr) {
            continue;
        }
        if (variant == Variant::wireframe && !terrain_comp->show_wireframe) {
            continue;
        }

        const TerrainAsset& terrain = *terrain_comp->terrain;
        const uint32_t patches = terrain.patches_per_side();
        if (patches == 0) {
            continue;
        }

        // Only the colour variant reads the material; the others push the
        // same block with the fallback row, which they never look at.
        gpu::GraphicsPipeline pipeline{};
        uint32_t material_id = fallback_material_id_;
        switch (variant) {
            case Variant::color:
                resolve_node_material(*terrain_comp, pipeline, material_id);
                break;
            case Variant::depth:
                pipeline = depth_pipeline_;
                break;
            case Variant::shadow:
                pipeline = shadow_pipeline_;
                break;
            case Variant::wireframe:
                pipeline = wireframe_pipeline_;
                break;
        }
        if (!pipeline.valid()) {
            continue;
        }

        if (pipeline != bound) {
            pass.set_pipeline(pipeline);
            pass.bind(ctx.globals);
            if (variant == Variant::color) pass.bind(ctx.bindless);
            bound = pipeline;
        }
        // The layer weights are read by the material's fragment stage alone.
        const gpu::BindEntry heights{"height_tex", gpu::sampled(terrain.height_texture(), sampler_)};
        const gpu::BindEntry bounds{"patch_bounds_tex",
                                    gpu::sampled(terrain.patch_bounds_texture(), sampler_)};
        if (variant == Variant::color) {
            pass.bind({heights, bounds,
                       {"weight_tex", gpu::sampled(terrain.weight_texture(), sampler_)}});
        } else {
            pass.bind({heights, bounds});
        }

        // The node's world translation positions the terrain, centred on the
        // pivot; the heightfield carries its own vertical placement on top.
        const auto* spatial = node->as_spatial();
        const glm::vec3 origin = terrain.world_min_corner(
            spatial != nullptr ? spatial->world_matrix_ : glm::mat4{1.0F});

        TerrainPushConstants push{};
        push.world_origin = {origin.x, origin.z};
        push.world_size = terrain.world_size();
        push.height_scale = terrain.height_scale();
        push.height_offset = terrain.height_offset() + origin.y;
        push.patches_per_side = patches;
        push.patch_quads = terrain.patch_quads();
        push.view_index = view_index;
        push.terrain_material_id = material_id;
        // Level selection is per view: the extent here is this view's, so a
        // shadow cascade covering a wide area picks coarser patches than the
        // camera does for the same ground.
        push.viewport = glm::vec2(extent);
        push.lod_thresholds = lod_thresholds_;
        for (uint32_t i = 0; i < 4; ++i) {
            push.layer_material_ids[static_cast<int>(i)] = terrain.layer_material_id(i);
            push.layer_uv_scales[static_cast<int>(i)] = terrain.layer_uv_scale(i);
        }
        pass.push(push);
        pass.draw_mesh_tasks(patches * patches, 1, 1);

        // Only the lit draw is counted: the stats panel reports what the
        // camera sees, and the depth, shadow and grid draws of the same
        // surface would read as several terrains on screen.
        if (variant == Variant::color && ctx.render_stats != nullptr) {
            ctx.render_stats->draw_calls++;
            const uint32_t dispatched = patches * patches;
            ctx.render_stats->terrain_patches_dispatched += dispatched;
            // Per patch: patch_quads² quads, two triangles each. The task
            // shader drops patches before the mesh stage runs, so this is the
            // ceiling rather than what rasterized.
            const uint32_t quads = terrain.patch_quads();
            ctx.render_stats->terrain_triangles_max += dispatched * quads * quads * 2;
        }
    }
}

void TerrainPass::record(const FrameContext& /*ctx*/) {}

void TerrainPass::draw(gpu::RenderEncoder& pass, const FrameContext& ctx) {
    if (ctx.scene == nullptr || ctx.target == nullptr) {
        return;
    }

    FJELL_PROFILE_SCOPE_N("record_terrain");
    auto zone = ctx.commands().zone("Terrain");
    const auto extent = ctx.target->extent();
    const glm::uvec2 size{extent.width, extent.height};
    draw_terrain_nodes(pass, ctx, Variant::color, 0, size);

    // Patch grid overlay, drawn per node that asks for it. A second walk
    // rather than a branch inside the first: the lines sit on top of the
    // shaded surface, so they are a separate draw regardless.
    draw_terrain_nodes(pass, ctx, Variant::wireframe, 0, size);
}

void TerrainPass::draw_shadow(gpu::RenderEncoder& pass, const FrameContext& ctx,
                              uint32_t cascade) {
    if (ctx.scene == nullptr) {
        return;
    }

    FJELL_PROFILE_SCOPE_N("record_terrain_shadow");
    draw_terrain_nodes(pass, ctx, Variant::shadow, cascade + 1,
                       glm::uvec2(ShadowPass::MAP_SIZE));
}

void TerrainPass::draw_depth(gpu::RenderEncoder& pass, const FrameContext& ctx) {
    if (ctx.scene == nullptr || ctx.target == nullptr) {
        return;
    }

    FJELL_PROFILE_SCOPE_N("record_terrain_depth");
    const auto extent = ctx.target->extent();
    draw_terrain_nodes(pass, ctx, Variant::depth, 0, {extent.width, extent.height});
}

} // namespace fjell
