#include "renderer/vulkan_context.hpp"
#include "gpu/vulkan/native.hpp"
#include "gpu/vulkan/translate.hpp"
#include "environment/environment_ubo.hpp"
#include "renderer/pass/cloud_shadow_pass.hpp"
#include "renderer/pass/fog_density_field_pass.hpp"
#include "renderer/pass/motion_field_pass.hpp"

#include "renderer/fjgraph_parser.hpp"
#include "renderer/pass_setup.hpp"
#include "renderer/render_settings.hpp"
#include "renderer/gpu/vk_check.hpp"
#include "renderer/gpu/vma_image.hpp"
#include "scripting/debug_draw_queue.hpp"
#include "animation/animation_system.hpp"
#include "renderer/components/mesh_component.hpp"
#include "renderer/components/terrain_component.hpp"
#include "renderer/resources/terrain_asset.hpp"
#include "renderer/components/skeletal_mesh_component.hpp"
#include "renderer/components/sprite_desc.hpp"
#include "renderer/resources/skeletal_model.hpp"
#include "ui/game_ui/game_ui.hpp"
#include "ui/game_ui/font_library.hpp"
#include "renderer/frame_graph.hpp"
#include "renderer/frustum.hpp"
#include "renderer/resources/material.hpp"
#include "renderer/resources/model.hpp"
#include "renderer/render_stats.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/uniform_types.hpp"
#include "renderer/resources/vertex.hpp"
#include "renderer/gpu/vk_utils.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "core/thread_pool.hpp"
#include "renderer/gpu/window.hpp"
#include "scene/scene.hpp"
#include "scene/light_node.hpp"

#include <imgui.h>
#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace fjell {

VulkanContext::VulkanContext(Window& window)
    : gpu_{window} {
    FJELL_GFX_INFO("Initializing Vulkan context");

    // Finalize global descriptor layout/pool/sets (builtins already declared
    // by GpuContext constructor; additional bindings could be declared here)
    gpu_.finalize_descriptors();

    // Scene gathering: indirect batches + draw data descriptors
    gatherer_.init(gpu_);
    gpu_.share_draw_data_layout(gatherer_.draw_data_layout());

    // Init pipelines now that the draw data layout is available
    gpu_.init_pipelines();

    PassResources pass_resources = gpu_.pass_resources();

    // Load the default render pipeline from `assets/render_graphs/default.fjgraph`.
    // Topology lives in data; pass behavior stays in C++ (PassFactory keys
    // class names → constructors, registered in pass_registrations.cpp).
    auto graph_result = FjGraphParser::load(
        base_path::resolve("assets/render_graphs/default.fjgraph"));
    if (!graph_result) {
        throw std::runtime_error("Failed to load default.fjgraph: " +
                                 graph_result.error());
    }
    auto create_result = create_passes_from_graph(
        *graph_result, passes_, render_pipeline_, gpu_, pass_resources);
    if (!create_result) {
        throw std::runtime_error(create_result.error());
    }
    loaded_graph_ = std::move(*graph_result);

    // The acceleration structures every ray tracer shares.
    if (gpu_device().caps().ray_queries) {
        accel_manager_ = std::make_unique<AccelStructureManager>(gpu_device());
        if (auto* rt_scene = passes_.try_get<RtScenePass>("rt_scene")) {
            rt_scene->set_accel_manager(accel_manager_.get());
        }
        ddgi_pass().set_accel_manager(accel_manager_.get());
        ss_gi_pass().set_accel_manager(accel_manager_.get());
        rt_reflection_pass().set_accel_manager(accel_manager_.get());
    }

    // Create render subsystems
    auto add_subsystem = [&](auto sys) {
        sys->create(gpu_.device_ref(), pass_resources);
        render_subsystems_.push_back(std::move(sys));
    };

    {
        auto ps = std::make_unique<ParticleSystem>();
        particle_system_ = ps.get();
        add_subsystem(std::move(ps));
    }
    {
        auto sr = std::make_unique<SpriteRenderer>();
        sprite_renderer_ = sr.get();
        add_subsystem(std::move(sr));
    }
    {
        auto tr = std::make_unique<TilemapRenderer>();
        tilemap_renderer_ = tr.get();
        add_subsystem(std::move(tr));
    }
    // Sort subsystems by transparent draw order (lower = drawn first = behind)
    std::sort(render_subsystems_.begin(), render_subsystems_.end(),
              [](const auto& a, const auto& b) {
                  return a->transparent_order() < b->transparent_order();
              });

    // Cache raw pointers for FrameContext
    subsystem_ptrs_.reserve(render_subsystems_.size());
    for (auto& s : render_subsystems_) {
        subsystem_ptrs_.push_back(s.get());
    }

    FJELL_GFX_DEBUG("Outline pipelines managed by PipelineCache");
    FJELL_GFX_DEBUG("Creating debug draw");
    debug_draw_ = std::make_unique<DebugDraw>();

    // The engine's own font is the one every UI falls back to.
    font_library_ = std::make_unique<FontLibrary>();
    font_library_->create(
        FontAtlasGpu{&gpu_.bindless_textures(), &gpu_device()},
        base_path::resolve("engine_assets/fonts/Inter.fjfont"));

    // Default post-process resources for the main viewport (all passes enabled)
    PostProcessConfig default_config{};
    default_config.ssr = true;
    default_config.volumetric_fog = true;
    default_config.ddgi = true;
    default_config.ss_gi = true;
    default_config.taa = true;
    default_config.rt_reflection = gpu_.device_ref().ray_tracing_supported();
    default_pp_ = std::make_unique<PostProcessResources>(gpu_device(), 256, 256, default_config,
                                                         PostProcessResources::PassRefs{ddgi_pass(), ss_gi_pass()});

    FJELL_GFX_INFO("Vulkan context ready");
}

VulkanContext::~VulkanContext() {
    default_pp_.reset();
    debug_draw_.reset();
    if (font_library_) font_library_->destroy();
    font_library_.reset();
    for (auto& sys : render_subsystems_) sys->destroy();
    render_subsystems_.clear();
    particle_system_ = nullptr;
    sprite_renderer_ = nullptr;
    tilemap_renderer_ = nullptr;
    accel_manager_.reset();

    passes_.destroy_all();
}

void VulkanContext::set_resource_manager(ResourceManager* rm) {
    resource_manager_ = rm;
    // The post pass builds its default look's material row itself, and fills
    // the shader's samplers with what an unset slot reads, the neutral table
    // for its grade among them.
    post_pass().set_texture_fallbacks(
        rm == nullptr ? PostPass::TextureFallbacks{}
                      : PostPass::TextureFallbacks{[rm](UniformHint hint) {
                            return rm->fallback_texture_index(hint);
                        }});
}

void VulkanContext::set_render_settings(const RenderSettings& s) {
    // MSAA intentionally not applied — requires pipeline + offscreen
    // recreation we don't support yet. Project's stored value becomes
    // effective on next engine launch via gpu_context's init-time read.

    // Global illumination
    ddgi_pass().config().enabled               = s.ddgi.enabled;
    ddgi_pass().config().gi_intensity          = s.ddgi.gi_intensity;
    ddgi_pass().config().infinite_bounces_mult = s.ddgi.infinite_bounces_mult;
    ddgi_pass().config().normal_bias           = s.ddgi.normal_bias;
    ddgi_pass().config().view_bias             = s.ddgi.view_bias;
    // Ray tracing falls back to the SDF trace when the device cannot do it,
    // so an unsupported project setting degrades rather than rendering
    // nothing.
    bool want_rt = s.ddgi.backend == RenderSettings::Ddgi::Backend::RayTraced
                && gpu_.ray_tracing_supported();
    ddgi_pass().set_backend(want_rt ? DdgiBackend::ray_traced
                                    : DdgiBackend::sdf_fallback);
    ddgi_requested_backend_ = s.ddgi.backend == RenderSettings::Ddgi::Backend::RayTraced
                                ? DdgiBackend::ray_traced
                                : DdgiBackend::sdf_fallback;
    // Changing this resizes every probe atlas, which Images::resize()
    // detects and rebuilds. Clamped because the cost is cubic and the
    // atlases have to stay within the device's 2D image limits.
    int pc = std::clamp(s.ddgi.probe_count, 4, 48);
    // Compared as a whole vector, not per axis: the grid is cubic by
    // definition here, so a config whose axes disagree with each other is
    // just as stale as one with the wrong count.
    glm::ivec3 wanted_counts(pc);
    if (ddgi_pass().config().probe_counts != wanted_counts) {
        ddgi_pass().config().probe_counts = wanted_counts;
        // The grid is re-divided, so the current fit no longer describes
        // it. Force the next bounds check to re-fit and discard.
        ddgi_grid_initialized_ = false;
        // Writing the config does not touch the atlases — only a resize
        // rebuilds them, and nothing calls one on a normal frame. Without
        // this the new count would sit inert until an unrelated viewport
        // resize or GI toggle happened to run.
        has_pending_ddgi_probe_count_ = true;
    }

    ss_gi_pass().config().enabled        = s.ss_gi.enabled;
    ss_gi_pass().config().intensity      = s.ss_gi.intensity;
    ss_gi_pass().config().max_trace_dist = s.ss_gi.max_trace_dist;
    ss_gi_pass().config().hysteresis     = s.ss_gi.hysteresis;

    // Post-processing
    gtao_pass().set_enabled(s.gtao.enabled);
    bloom_pass().set_enabled(s.bloom.enabled);
    // RT mode requires hardware support; fall back to ScreenSpace when unavailable
    // so a project authored on an RT-capable machine still renders on others.
    auto requested_mode = s.ssr.mode;
    if (requested_mode == RenderSettings::Ssr::Mode::RayTraced && !gpu_.device_ref().ray_tracing_supported()) {
        requested_mode = RenderSettings::Ssr::Mode::ScreenSpace;
    }
    ssr_pass().set_mode(requested_mode);
    ssr_pass().set_intensity(s.ssr.intensity);
    ssr_pass().set_thickness(s.ssr.thickness);
    rt_reflection_pass().set_mode(requested_mode);
    rt_reflection_pass().set_intensity(s.ssr.intensity);
    rt_reflection_pass().set_max_trace_dist(s.ssr.max_trace_dist);
    sun_shafts_pass().set_upsample_depth_tolerance(s.sun_shafts.upsample_depth_tolerance);

    // Exposure
    post_pass().set_auto_exposure(s.tonemap.auto_exposure);
    post_pass().set_exposure(s.tonemap.exposure_ev);
    post_pass().set_adaptation_speed(s.tonemap.adaptation_speed);

    // Frame graph
    render_pipeline_.set_aliasing_enabled(s.frame_graph_aliasing);
    async_compute_enabled_ = s.async_compute;
}

void VulkanContext::sync_sprite_textures(Scene& scene, ResourceManager& resources, const Project* project) {
    sprite_renderer_->sync_textures(scene, resources, project);
}

void VulkanContext::sync_tilemap_textures(Scene& scene, ResourceManager& resources, const Project* project) {
    tilemap_renderer_->sync_textures(scene, resources, project);
}

// ── .fjgraph cache ──────────────────────────────────────────────────────

const FjGraph* VulkanContext::get_or_load_graph(const std::string& path) {
    auto it = graph_cache_.find(path);
    if (it != graph_cache_.end()) { return &it->second; }

    auto resolved = base_path::resolve(path);
    auto loaded = FjGraphParser::load(resolved);
    if (!loaded) {
        FJELL_GFX_ERROR("Failed to load .fjgraph '{}': {}", path, loaded.error());
        return nullptr;
    }

    auto [inserted_it, _] = graph_cache_.emplace(path, std::move(*loaded));
    FJELL_GFX_DEBUG("VulkanContext: cached .fjgraph '{}'", path);
    return &inserted_it->second;
}

// ── Hot-reload ──────────────────────────────────────────────────────────

namespace {

/// Whether a render call governed by `graph` runs the pass registered under
/// `name`. A null graph means no per-viewport mask applies and the global
/// pass state decides, so everything counts as running. Mirrors the
/// membership test `RenderPipeline::execute` uses to build its mask.
[[nodiscard]] bool runs_pass(const FjGraph* graph, std::string_view name) {
    if (graph == nullptr) { return true; }
    return std::ranges::any_of(graph->passes, [&](const FjGraphPass& p) {
        return p.enabled && p.name == name;
    });
}

// Compare params field-by-field. PassParams is small (3-5 keys per pass);
// O(n*m) is fine.
bool params_equal(const PassParams& a, const PassParams& b) {
    auto count = [](const PassParams& p) {
        size_t n = 0;
        for ([[maybe_unused]] const auto& _ : p) { ++n; }
        return n;
    };
    if (count(a) != count(b)) { return false; }
    for (const auto& [k, v] : a) {
        const auto* bv = b.find(k);
        if (bv == nullptr || *bv != v) { return false; }
    }
    return true;
}

bool entries_shape_equal(const FjGraphPass& a, const FjGraphPass& b) {
    return a.class_name == b.class_name &&
           a.name       == b.name &&
           a.enabled_if == b.enabled_if &&
           params_equal(a.params, b.params);
}

} // namespace

void VulkanContext::reload_fjgraph(const std::string& path) {
    auto reparsed = FjGraphParser::load(path);
    if (!reparsed) {
        FJELL_GFX_ERROR("FjGraphWatcher: parse failed: {}", reparsed.error());
        return;
    }

    // Shape diff: same number of entries, identical class/name/params/queue/phase
    // for each. Only `enabled` may differ. Anything else needs restart.
    bool shape_ok = reparsed->passes.size() == loaded_graph_.passes.size();
    if (shape_ok) {
        for (size_t i = 0; i < reparsed->passes.size(); ++i) {
            if (!entries_shape_equal(reparsed->passes[i],
                                     loaded_graph_.passes[i])) {
                shape_ok = false;
                break;
            }
        }
    }

    if (!shape_ok) {
        FJELL_GFX_WARN(
            "FjGraphWatcher: pass shape changed (adds/removes/params/queue) — "
            "restart the engine to apply. Saved graph remains parsed; only "
            "'enabled' flag changes are honored live in v1.");
        return;
    }

    apply_enabled_flags(*reparsed, render_pipeline_);
    loaded_graph_ = std::move(*reparsed);
    FJELL_GFX_INFO("FjGraphWatcher: applied enable-flag changes from {}", path);
}

// ── Command buffer recording ────────────────────────────────────────────

FrameContext VulkanContext::build_frame_context(VkCommandBuffer cmd, const ViewportRenderRequest& view,
                                                 Scene* scene, float dt, uint32_t selected_id,
                                                 PostProcessResources* pp) {
    OffscreenPass& target = *view.target;
    FrameContext ctx{};
    ctx.cmd = cmd;
    // Async compute CB is set on the main-viewport path in render_frame
    // (after build_frame_context returns). Preview viewports never hit
    // the async path — they submit their own single-CB bundle.
    ctx.target = &target;
    ctx.queue = &gatherer_.render_queue();
    ctx.camera_position = gatherer_.camera_position();
    // The camera this viewport is drawn from, for a pass that has to place
    // something in the view before recording it. The same matrices feed the
    // global UBO; a pass reconstructing from depth still wants inv_view_proj
    // below, which carries the TAA jitter.
    ctx.view = gatherer_.view_matrix();
    ctx.projection = gatherer_.projection_matrix();
    // Previews bind their own global set; everyone else the frame slot's.
    ctx.globals = view.global_group.valid() ? view.global_group
                                             : gpu_.global_bindings().group(current_frame_);
    ctx.bindless = gpu_.bindless_textures().group(current_frame_);
    ctx.draw_data = gatherer_.draw_data_descriptor().group(current_frame_);
    ctx.debug_draw = debug_draw_.get();
    ctx.scene = scene;
    ctx.environment = current_environment_;
    ctx.delta_time = dt;
    ctx.elapsed_time = elapsed_time_;
    ctx.selected_id = selected_id;
    ctx.show_colliders = show_colliders_;
    ctx.frame_index = current_frame_;
    ctx.indirect_batch = gatherer_.indirect_batch();
    ctx.mega_buffer = gpu_.mega_buffer();
    ctx.pipeline_cache = gpu_.pipeline_cache();
    // The color passes draw indirect from these buffers, so they may only be
    // handed over when the meshlet cull passes actually run in this call and
    // fill them. A viewport graph that leaves the cull passes out would
    // otherwise dispatch mesh tasks off whatever the buffers last held, which
    // faults the device rather than drawing nothing.
    if (passes_.has("meshlet_cull") && runs_pass(view.viewport_graph, "meshlet_cull")) {
        ctx.meshlets = gpu_.meshlet_descriptor().group(current_frame_);
        ctx.gpu_driven_commands = meshlet_instance_pass()->indirect_buffer(current_frame_);
    }
    ctx.pp_resources = pp;
    ctx.descriptor_cache = &gpu_.descriptor_cache();
    ctx.device = &gpu_.core().gpu_device();
    ctx.transient_pool = &gpu_.transient_pool();
    ctx.registry = &resource_registry_;
    ctx.skip_shadows = view.skip_shadows;
    ctx.backdrop = view.backdrop;
    ctx.show_grid = view.show_grid;

    // Read inv_view_proj from the already-written GlobalUbo
    const auto* ubo_data =
        reinterpret_cast<const GlobalUbo*>(gpu_device().mapped(gpu_.uniform_buffer(current_frame_)).data());
    ctx.inv_view_proj = ubo_data->inv_view_proj;

    // Extended context for self-registering passes
    ctx.render_stats = &gpu_.render_stats();
    ctx.subsystems = subsystem_ptrs_.data();
    ctx.subsystem_count = static_cast<uint32_t>(subsystem_ptrs_.size());
    ctx.active_sources = active_sources_;
    ctx.passes = &passes_;
    ctx.playing = playing_;
    ctx.world_ui_callback = world_ui_callback_;

    // Shadow cascade batches (filled from gatherer, consumed by ShadowPass::build)
    for (uint32_t c = 0; c < FrameContext::MAX_SHADOW_CASCADES; ++c) {
        ctx.shadow_cascade_batches[c] = gatherer_.shadow_batch(c);
        ctx.shadow_cascade_draw_data_groups[c] = gatherer_.shadow_draw_data(c).group(current_frame_);
        ctx.shadow_cascade_commands[c] = &gatherer_.shadow_cmds(c);
    }

    return ctx;
}

void VulkanContext::prepare_ray_tracing(const std::vector<ViewportRenderRequest>& viewports,
                                        Scene* scene) {
    if (!accel_manager_ || scene == nullptr) return;

    // How far each active tracer's rays go, from every viewport rendering the
    // scene: the TLAS is built once for all of them, so it covers the union.
    AABB reach;
    const auto add = [&reach](const AABB& box) {
        if (!box.valid()) return;
        reach.expand(box.min);
        reach.expand(box.max);
    };
    if (default_pp_ && default_pp_->config().ddgi && ddgi_pass().config().enabled
        && ddgi_pass().backend() == DdgiBackend::ray_traced) {
        add(ddgi_pass().trace_reach_bounds());
    }
    for (const auto& vp : viewports) {
        if (!vp.target) continue;
        if (rt_reflection_pass().ray_traced_active()) {
            add(rt_reflection_pass().trace_reach_bounds(vp.camera_position));
        }
        if (ss_gi_pass().config().enabled) {
            add(ss_gi_pass().trace_reach_bounds(vp.camera_position));
        }
    }
    // Nothing traces this frame, so no TLAS: a tracer asking finds none.
    if (!reach.valid()) return;

    gatherer_.gather_rt(*scene, reach);
    accel_manager_->prepare_scene(*scene, gatherer_.rt_commands(), gpu_.pipeline_cache(),
                                  particle_system_, *gpu_.mega_buffer(), current_frame_);
}

void VulkanContext::render_scene_to(VkCommandBuffer cmd, const ViewportRenderRequest& view,
                                    Scene* scene, float dt, uint32_t selected_id,
                                    ThreadPool& pool, PostProcessResources* pp) {
    FJELL_PROFILE_SCOPE_N("render_scene_to");
    OffscreenPass& target = *view.target;
    const FjGraph* viewport_graph = view.viewport_graph;

    // Gather scene: frustum cull, build render queue and indirect batches
    {
        GatherInput gi{};
        gi.scene = scene;
        gi.view = gatherer_.view_matrix();
        gi.projection = gatherer_.projection_matrix();
        gi.camera_position = gatherer_.camera_position();
        gi.frame_index = current_frame_;
        gi.shadow_pass = &shadow_pass();
        gi.pool = &pool;
        gi.stats = &gpu_.render_stats();
        gi.animation_system = animation_system_;
        gi.pipelines = gpu_.pipeline_cache();
        gatherer_.gather(gi);
    }

    // What the table gained or changed since this slot's last frame
    gpu_.bindless_textures().flush(current_frame_);

    // Update render subsystems (gather data, upload SSBOs)
    {
        RenderSubsystemContext sub_ctx{};
        sub_ctx.scene = scene;
        sub_ctx.resource_manager = resource_manager_;
        sub_ctx.project = project_;
        sub_ctx.view_projection = gatherer_.projection_matrix() * gatherer_.view_matrix();
        sub_ctx.camera_position = gatherer_.camera_position();
        sub_ctx.frame_index = current_frame_;
        sub_ctx.dt = dt;
        sub_ctx.playing = playing_;
        for (auto& sys : render_subsystems_) sys->update(sub_ctx);
    }

    // Per-frame pass setup (buffer resize, data upload, etc.)
    // Must run before build_frame_context since ensure_capacity may
    // reallocate buffers whose handles are captured into FrameContext.
    {
        FrameContext pre_ctx{};
        pre_ctx.frame_index = current_frame_;
        pre_ctx.environment = current_environment_;
        pre_ctx.indirect_batch = gatherer_.indirect_batch();
        pre_ctx.pp_resources = pp;
        pre_ctx.target = &target;
        pre_ctx.queue = &gatherer_.render_queue();
        pre_ctx.mega_buffer = gpu_.mega_buffer();
        passes_.pre_record_all(pre_ctx, gpu_);
    }

    // Update meshlet descriptor (set 3) — must be after pre_record (ensure_capacity)
    // so buffer handles are stable.
    bool has_meshlet_passes = passes_.has("meshlet_cull");
    if (has_meshlet_passes && gpu_.meshlet_buffer() && gpu_.meshlet_buffer()->total_meshlets() > 0) {
        gpu_.meshlet_descriptor().update(*gpu_.meshlet_buffer(), *gpu_.mega_buffer(), current_frame_,
                                         meshlet_instance_pass()->visible_buffer(current_frame_),
                                         meshlet_instance_pass()->visible_buffer_size(current_frame_));
    }

    current_ctx_ = build_frame_context(cmd, view, scene, dt, selected_id, pp);
    // Hand the async CB + post-compute graphics CB to the graph, but
    // only for the main frame CB — preview viewports get their own
    // one-shot CBs from render_preview and have no paired compute/post
    // CBs, so routing their passes through the main frame's async CB
    // would interleave preview compute dispatches into a CB that
    // submit_and_present fences independently. Nulling both handles
    // folds previews back to single-CB behavior.
    const bool is_main_cb = (cmd == frame_cmd_);
    current_ctx_.async_compute_cmd = is_main_cb ? async_cmd_ : VK_NULL_HANDLE;
    current_ctx_.graphics_post_cmd = is_main_cb ? post_cmd_ : VK_NULL_HANDLE;
    // The main frame binds the per-frame set, where record_frame put the GI
    // outputs; a preview's set holds the fallbacks there.
    current_ctx_.bound_gi_indirect = is_main_cb ? bound_gi_indirect_ : gpu::Texture{};
    current_ctx_.bound_ss_gi_emissive = is_main_cb ? bound_ss_gi_emissive_ : gpu::Texture{};

    // Populate resource registry — passes self-register their outputs,
    // then non-pass resources (meshlet bounds) are added manually.
    resource_registry_.clear();
    passes_.register_resources_all(resource_registry_);
    if (gpu_.meshlet_buffer()) {
        resource_registry_.register_buffer("meshlet_bounds", {
            gpu_.meshlet_buffer()->bounds_buffer(),
            gpu_.meshlet_buffer()->bounds_buffer_size()});
    }

    // The project decides whether compute-hinted passes leave the graphics
    // queue (Project Settings, Frame graph); the device decides whether
    // they can; and only the main frame has a compute command buffer to put
    // them in. A preview's are graphics passes, and the graph must sync them
    // as such.
    const bool async_available = async_compute_enabled_
                              && gpu_.device_ref().async_compute_supported()
                              && current_ctx_.async_compute_cmd != VK_NULL_HANDLE;
    bool any_async = render_pipeline_.execute(
        cmd, target, target.samples(), active_sources_,
        pool, gpu_.secondary_pools(), current_frame_,
        resource_registry_, passes_, pp, current_ctx_,
        async_available,
        viewport_graph);
    // Accumulate across viewports — a frame uses async if *any* viewport
    // records to the async CB. Preview viewports have null
    // async_compute_cmd so they always return false.
    recorded_async_this_frame_ = recorded_async_this_frame_ || any_async;

    // Post-pipeline cleanup (SSR history copy, etc.)
    passes_.post_record_all(current_ctx_);
}

void VulkanContext::render_preview(const ViewportRenderRequest& req,
                                    const EnvironmentState& environment, Scene* scene,
                                    float dt, uint32_t selected_id, ThreadPool& pool) {
    FJELL_PROFILE_SCOPE_N("render_preview");
    if (!req.target) return;
    if (!req.global_group.valid()) {
        FJELL_GFX_ERROR("A preview needs a global set of its own "
                        "(GlobalBindings::make_view_group); it is not drawn");
        return;
    }
    current_environment_ = &environment;

    auto vk = gpu_.device();
    VkCommandBuffer cmd{};
    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = gpu_.command_pool();
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;
    vkAllocateCommandBuffers(vk, &alloc_info, &cmd);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    auto* pp = req.pp_resources ? req.pp_resources : default_pp_.get();

    // The preview's own global set takes what the frame slot's holds, with
    // this view's depth, AO, contact shadows and screen colour in place of
    // the frame's.
    {
        // AO and contact shadows are what this view's run computes: its own
        // AO image, and the shared contact shadow image once its run has
        // traced it. A view that runs neither reads nothing occluding, not
        // the main view's, which lie over other pixels.
        const gpu::BindResource unoccluded = gpu::sampled(gpu_.unoccluded(), gpu_.gi_fallback_sampler());
        const auto* view_ao = req.pp_resources != nullptr ? req.pp_resources->gtao() : nullptr;
        const bool traces_contact = req.viewport_graph != nullptr
            ? runs_pass(req.viewport_graph, "contact_shadow")
            : render_pipeline_.is_pass_enabled("contact_shadow");
        const gpu::BindResource gi_fallback = gpu::sampled(gpu_.gi_fallback(), gpu_.gi_fallback_sampler());
        const gpu::BindEntry view_images[] = {
            {"depth_texture", gpu::sampled(req.target->depth_texture(), gpu_.depth_sampler())},
            {"ao_texture", view_ao != nullptr && view_ao->ao ? gpu::sampled(view_ao->ao, gtao_pass().ao_sampler())
                                                            : unoccluded},
            {"screen_color", gpu::sampled(req.target->screen_color_texture(), post_pass().output_sampler())},
            {"gi_indirect", gi_fallback},
            {"ss_gi_emissive", gi_fallback},
            {"contact_shadow", traces_contact ? gpu::sampled(contact_shadow_pass().shadow(),
                                                             contact_shadow_pass().shadow_sampler())
                                              : unoccluded},
        };
        gpu_.global_bindings().refresh_view(req.global_group, current_frame_, view_images);
    }

    update_uniform_buffer(current_frame_, req.view, req.projection,
                          req.camera_position, environment, scene, req.target->extent(), *pp);
    update_light_ssbo(current_frame_, scene);
    update_fog_volume_ssbo(current_frame_);
    render_scene_to(cmd, req, scene, dt, selected_id, pool, pp);

    vkEndCommandBuffer(cmd);

    // This one-shot submit reads meshes/textures that may still sit in the
    // upload batch (it doesn't wait the upload timeline like the frame
    // submit does) — settle them first. Cheap when nothing is pending.
    gpu_.upload_context().wait_all();

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    if (VkResult r = vkQueueSubmit(gpu_.graphics_queue(), 1, &submit, VK_NULL_HANDLE);
        r != VK_SUCCESS) {
        FJELL_GFX_CRITICAL("render_preview vkQueueSubmit failed: VkResult={}", static_cast<int>(r));
        if (r == VK_ERROR_DEVICE_LOST) gpu_.device_ref().dump_device_fault("preview submit");
    }
    {
        FJELL_PROFILE_SCOPE_N("render_preview_wait");
        if (VkResult r = vkQueueWaitIdle(gpu_.graphics_queue()); r != VK_SUCCESS) {
            FJELL_GFX_CRITICAL("render_preview vkQueueWaitIdle failed: VkResult={}", static_cast<int>(r));
            if (r == VK_ERROR_DEVICE_LOST) gpu_.device_ref().dump_device_fault("preview wait idle");
        }
    }

    vkFreeCommandBuffers(vk, gpu_.command_pool(), 1, &cmd);
}

void VulkanContext::record_command_buffer(VkCommandBuffer cmd, const gpu::SwapchainImage& image,
                                          const std::vector<ViewportRenderRequest>& viewports,
                                          const EnvironmentState& environment,
                                          Scene* scene, float dt,
                                          uint32_t selected_id, ThreadPool& pool) {
    FJELL_PROFILE_SCOPE_N("record_command_buffer");
    current_environment_ = &environment;

    // Once-per-frame pass preparation: each pass scans the scene for the
    // nodes it needs (shadow cascades, fog params, sky colors, etc.)
    {
        FrameContext prep_ctx{};
        prep_ctx.scene = scene;
        prep_ctx.environment = &environment;
        prep_ctx.frame_index = current_frame_;
        // The clock the global UBO's `time` carries, already advanced for this
        // frame, for a pass that steps something on it before recording.
        prep_ctx.elapsed_time = elapsed_time_;
        // Terrain uploads its ray mesh here: this is the pass hook that carries
        // the scene, and geometry has to reach the mega-buffer before the TLAS
        // is built during recording.
        prep_ctx.mega_buffer = gpu_.mega_buffer();
        // For a pass that configures another before anything records: the
        // post pass switches the exposure histogram on or off here.
        prep_ctx.passes = &passes_;
        if (!viewports.empty() && viewports[0].target) {
            prep_ctx.view = viewports[0].view;
            prep_ctx.projection = viewports[0].projection;
            prep_ctx.camera_position = viewports[0].camera_position;
            prep_ctx.target = viewports[0].target;
        }
        passes_.prepare_frame_all(prep_ctx, gpu_);
    }


    prepare_ray_tracing(viewports, scene);

    // --- Render scene once per viewport ---
    for (const auto& vp : viewports) {
        if (!vp.target) continue;
        auto* pp = vp.pp_resources ? vp.pp_resources : default_pp_.get();
        update_uniform_buffer(current_frame_, vp.view, vp.projection,
                              vp.camera_position, environment, scene, vp.target->extent(), *pp);
        update_light_ssbo(current_frame_, scene);
        update_fog_volume_ssbo(current_frame_);
        // The frame's own viewports bind the per-frame set and are shown the
        // right way up, whatever a request says.
        ViewportRenderRequest view = vp;
        view.global_group = {};
        render_scene_to(cmd, view, scene, dt, selected_id, pool, pp);
    }

    // --- UI editor preview offscreen pass ---
    if (ui_preview_callback_) {
        ui_preview_callback_(current_frame_);
    }

    // --- Play mode HUD offscreen pass ---
    if (play_hud_callback_) {
        play_hud_callback_(current_frame_);
    }

    // ImGui to swapchain runs post-compute: on frames with async work
    // it needs to wait for both the graphics pre-CB and compute CB to
    // finish. Easiest is to record into the post-compute CB when the
    // frame uses async (post_cmd non-null + recorded_async_this_frame_);
    // otherwise stick with the pre CB, same as today's single-CB flow.
    gpu::CommandList& swap_list = (post_list_ != nullptr && recorded_async_this_frame_)
        ? *post_list_ : *pre_list_;
    VkCommandBuffer swap_cmd = gpu::vulkan::native_command_buffer(swap_list);

    // --- Render ImGui to swapchain ---
    {
        const gpu::Transition draw = image.to_draw();
        swap_list.transition({&draw, 1});
        {
            auto target = swap_list.render({.color = {{.view = image.texture,
                                                       .load = gpu::Load::clear,
                                                       .clear = gpu::Clear{0.012f, 0.012f, 0.015f, 1.0f}}}});
            if (overlay_callback_) {
                overlay_callback_(target);
            }
        }
        const gpu::Transition show = image.to_present();
        swap_list.transition({&show, 1});
    }

    gpu::vulkan::collect_zones(gpu_.core().gpu_device(), swap_cmd);
}

static float halton(int index, int base) {
    float f = 1.0f;
    float r = 0.0f;
    int i = index;
    while (i > 0) {
        f /= static_cast<float>(base);
        r += f * static_cast<float>(i % base);
        i /= base;
    }
    return r;
}

void VulkanContext::update_uniform_buffer(uint32_t frame_index,
                                          const glm::mat4& view,
                                          const glm::mat4& projection,
                                          const glm::vec3& camera_position,
                                          const EnvironmentState& environment,
                                          Scene* scene, glm::uvec2 extent,
                                          PostProcessResources& pp) {
    FJELL_PROFILE_SCOPE_N("update_uniform_buffer");

    ViewHistory& history = pp.view_history();

    // TAA sub-pixel jitter: offset the projection matrix by ±0.5 pixels
    bool taa_enabled = pp.config().taa;
    glm::mat4 jittered_projection = projection;
    float jx = 0.0f;
    float jy = 0.0f;
    if (taa_enabled && extent.x > 0 && extent.y > 0) {
        int idx = static_cast<int>((history.taa_frame_count++) % 16) + 1; // 1-based, 16-sample cycle
        jx = (halton(idx, 2) - 0.5f) / static_cast<float>(extent.x);
        jy = (halton(idx, 3) - 0.5f) / static_cast<float>(extent.y);
        // Apply jitter as a translation in clip space (NDC offset).
        // Perspective: w = -view_z, so column 2 produces a constant NDC shift after divide.
        // Orthographic: w = 1, so the same column scales with view-space z (depth-dependent
        // shear). Use the translation column instead, where w = 1 unconditionally.
        const bool is_perspective = projection[3][3] == 0.0f;
        if (is_perspective) {
            jittered_projection[2][0] += jx * 2.0f;
            jittered_projection[2][1] += jy * 2.0f;
        } else {
            jittered_projection[3][0] += jx * 2.0f;
            jittered_projection[3][1] += jy * 2.0f;
        }
    }

    gatherer_.set_camera_state(camera_position, view, jittered_projection);
    GlobalUbo ubo{};
    ubo.view = view;
    ubo.projection = jittered_projection;
    ubo.unjittered_vp = projection * view;
    ubo.inv_unjittered_vp = glm::inverse(ubo.unjittered_vp);
    ubo.prev_unjittered_vp = history.unjittered_view_proj;
    ubo.jitter_x = jx;
    ubo.jitter_y = jy;
    // Point and spot lights are counted here; the directional light comes
    // resolved from the environment.
    uint32_t point_count = 0;
    uint32_t spot_count = 0;
    if (scene) {
        scene->each_node<PointLightNode>([&](const PointLightNode&) { ++point_count; });
        scene->each_node<SpotLightNode>([&](const SpotLightNode&) { ++spot_count; });
        // Also count legacy lights
        point_count += static_cast<uint32_t>(scene->point_lights().size());
        spot_count += static_cast<uint32_t>(scene->spot_lights().size());
    }
    if (point_count > 1024) {
        FJELL_GFX_WARN("Point light overflow: {} lights, max 1024 — extras ignored", point_count);
        point_count = 1024;
    }
    if (spot_count > 1024) {
        FJELL_GFX_WARN("Spot light overflow: {} lights, max 1024 — extras ignored", spot_count);
        spot_count = 1024;
    }

    const DirectionalBody& sun = environment.primary;
    ubo.light_direction = glm::vec4(sun.direction, sun.intensity);
    ubo.light_color = glm::vec4(sun.color, 0.0f);
    ubo.camera_position = glm::vec4(camera_position, 0.0f);
    ubo.ambient_color = glm::vec4(environment.ambient_color, environment.sky_light_intensity());

    // Forward sun data to the trace passes for closest-hit direct lighting;
    // they take the direction the light travels.
    ss_gi_pass().set_sun_light(-sun.direction, sun.color, sun.intensity);
    rt_reflection_pass().set_sun_light(-sun.direction, sun.color, sun.intensity);

    // Keep DDGI's copy of the SDF bounds current for the RT burial test —
    // the volume can move while the probe grid stays frozen. Water takes the
    // same pair, to foam where it meets solid geometry; one source so the two
    // readers cannot disagree about where the volume is.
    ddgi_pass().set_sdf_bounds(scene_sdf_pass().volume_min(), scene_sdf_pass().volume_max());
    water_pass().set_sdf_bounds(scene_sdf_pass().volume_min(), scene_sdf_pass().volume_max());

    const auto& matrices = shadow_pass().light_matrices();
    const auto& splits = shadow_pass().cascade_splits();
    for (uint32_t i = 0; i < ShadowPass::CASCADE_COUNT; ++i) {
        ubo.cascade_vp[i] = matrices[i];
    }
    ubo.cascade_splits = glm::vec4(splits[0], splits[1], splits[2], 0.0f);
    // DDGI lights this viewport's surfaces only if it runs DDGI: the image
    // the frame binds may be another viewport's.
    ubo.ddgi_weight = pp.config().ddgi ? 1.0f : 0.0f;
    // SS-GI: emissive color bleeding (supplements DDGI)
    if (pp.config().ss_gi && pp.ss_gi() && pp.ss_gi()->written) {
        ubo.ss_gi_intensity = ss_gi_pass().config().intensity;
    } else {
        ubo.ss_gi_intensity = 0.0f;
    }
    ubo.debug_light_channel = debug_light_channel_;

    // Forward+ tile info
    uint32_t tiles_x = (extent.x + LIGHT_TILE_SIZE - 1) / LIGHT_TILE_SIZE;
    uint32_t tiles_y = (extent.y + LIGHT_TILE_SIZE - 1) / LIGHT_TILE_SIZE;
    ubo.tile_info = glm::uvec4(tiles_x, tiles_y, point_count, spot_count);
    ubo.inv_projection = glm::inverse(jittered_projection);
    ubo.inv_view_proj = glm::inverse(jittered_projection * view);
    ubo.prev_view_proj = history.view_proj;
    ubo.prev_view = history.view;
    ubo.time = elapsed_time_;

    std::memcpy(gpu_device().mapped(gpu_.uniform_buffer(frame_index)).data(), &ubo, sizeof(ubo));
    EnvironmentUbo env_ubo = make_environment_ubo(environment);
    // The cloud shadow map's placement is the renderer's, following the
    // camera, so it joins the block here rather than in the resolved state.
    if (auto* clouds = passes_.try_get<CloudShadowPass>("cloud_shadow")) {
        env_ubo.cloud_shadow_vp = clouds->world_to_uv();
        env_ubo.cloud_shadow_texture = clouds->active() ? 1u : 0u;
    }
    if (auto* motion = passes_.try_get<MotionFieldPass>("motion_field")) {
        env_ubo.motion_field_vp = motion->world_to_uv();
        env_ubo.motion_field_texture = motion->active() ? 1u : 0u;
    }
    if (auto* density = passes_.try_get<FogDensityFieldPass>("fog_density_field")) {
        env_ubo.fog_density_field_vp = density->world_to_uv();
        env_ubo.fog_density_field_texture = density->active() ? 1u : 0u;
    }
    std::memcpy(gpu_device().mapped(gpu_.environment_buffer(frame_index)).data(), &env_ubo, sizeof(env_ubo));

    history.advance(view, jittered_projection, projection);
}

void VulkanContext::update_light_ssbo(uint32_t frame_index, Scene* scene) {
    FJELL_PROFILE_SCOPE_N("update_light_ssbo");
    if (!scene) return;

    // Build lookup from light index → shadow tile/block
    const auto& spot_tiles = local_shadow_pass().spot_tiles();
    std::unordered_map<uint32_t, const ShadowTile*> spot_shadow_map;
    for (const auto& tile : spot_tiles) {
        spot_shadow_map[tile.light_index] = &tile;
    }

    const auto& point_blocks = local_shadow_pass().point_blocks();
    std::unordered_map<uint32_t, const LocalShadowPass::PointShadowBlock*> point_shadow_map;
    for (const auto& block : point_blocks) {
        point_shadow_map[block.light_index] = &block;
    }

    auto* dst = reinterpret_cast<uint8_t*>(gpu_device().mapped(gpu_.light_ssbo(frame_index)).data());
    uint32_t point_count = 0;
    uint32_t spot_count = 0;

    // Gather lights from scene nodes (position from world transform). A
    // light's index here is its place in its kind's list, which is how the
    // shadow atlas names it too (LocalShadowPass).
    scene->each_node<PointLightNode>([&](const PointLightNode& light) {
        const PointLightNode* pl = &light;
        if (point_count >= 1024) return;
        GpuPointLight gpu{};
        glm::vec3 pos = glm::vec3(pl->world_matrix_[3]);
        gpu.position_radius = glm::vec4(pos, pl->radius);
        gpu.color_intensity = glm::vec4(pl->color, pl->intensity);
        gpu.vol_params = glm::vec4(pl->vol_scatter_intensity, 0.0f, 0.0f, 0.0f);
        // Fill shadow block data if this point light has shadows assigned
        auto pit = point_shadow_map.find(point_count);
        if (pit != point_shadow_map.end()) {
            const auto* blk = pit->second;
            gpu.shadow_info = glm::ivec4(
                static_cast<int>(blk->x),
                static_cast<int>(blk->y),
                static_cast<int>(blk->tile_size),
                1); // flags: 1 = has shadow
        } else {
            gpu.shadow_info = glm::ivec4(0);
        }
        std::memcpy(dst + point_count * sizeof(GpuPointLight), &gpu, sizeof(GpuPointLight));
        ++point_count;
    });
    scene->each_node<SpotLightNode>([&](const SpotLightNode& light) {
        const SpotLightNode* sl = &light;
        if (spot_count >= 1024) return;
        GpuSpotLight gpu{};
        glm::vec3 pos = glm::vec3(sl->world_matrix_[3]);
        gpu.position_radius = glm::vec4(pos, sl->radius);
        // Transform direction by world rotation so parenting/rotation works
        glm::vec3 world_dir = glm::normalize(glm::mat3(sl->world_matrix_) * sl->direction);
        gpu.direction_inner_cos = glm::vec4(world_dir,
                                             std::cos(glm::radians(sl->inner_angle)));
        gpu.color_intensity = glm::vec4(sl->color, sl->intensity);
        gpu.outer_cos = std::cos(glm::radians(sl->outer_angle));
        gpu.vol_params = glm::vec4(sl->vol_scatter_intensity, 0.0f, 0.0f, 0.0f);
        // Fill shadow tile data if this spot light has shadows assigned
        auto it = spot_shadow_map.find(spot_count);
        if (it != spot_shadow_map.end()) {
            const auto* tile = it->second;
            gpu.shadow_atlas_x = static_cast<int>(tile->x);
            gpu.shadow_atlas_y = static_cast<int>(tile->y);
            gpu.shadow_tile_size = static_cast<int>(tile->size);
            gpu.shadow_vp = tile->light_vp;
        } else {
            gpu.shadow_atlas_x = 0;
            gpu.shadow_atlas_y = 0;
            gpu.shadow_tile_size = 0;
            gpu.shadow_vp = glm::mat4(1.0f);
        }
        auto* spot_dst = dst + 1024 * sizeof(GpuPointLight);
        std::memcpy(spot_dst + spot_count * sizeof(GpuSpotLight), &gpu, sizeof(GpuSpotLight));
        ++spot_count;
    });

    // Legacy: also include lights from the old separate vectors
    const auto& old_point_lights = scene->point_lights();
    for (uint32_t i = 0; i < static_cast<uint32_t>(old_point_lights.size()) && point_count < 1024; ++i) {
        const auto& pl = old_point_lights[i];
        GpuPointLight gpu{};
        gpu.position_radius = glm::vec4(pl.position, pl.radius);
        gpu.color_intensity = glm::vec4(pl.color, pl.intensity);
        gpu.vol_params = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
        std::memcpy(dst + point_count * sizeof(GpuPointLight), &gpu, sizeof(GpuPointLight));
        ++point_count;
    }

    const auto& old_spot_lights = scene->spot_lights();
    auto* spot_dst = dst + 1024 * sizeof(GpuPointLight);
    for (uint32_t i = 0; i < static_cast<uint32_t>(old_spot_lights.size()) && spot_count < 1024; ++i) {
        const auto& sl = old_spot_lights[i];
        GpuSpotLight gpu{};
        gpu.position_radius = glm::vec4(sl.position, sl.radius);
        gpu.direction_inner_cos = glm::vec4(glm::normalize(sl.direction),
                                             std::cos(glm::radians(sl.inner_angle)));
        gpu.color_intensity = glm::vec4(sl.color, sl.intensity);
        gpu.outer_cos = std::cos(glm::radians(sl.outer_angle));
        gpu.vol_params = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
        std::memcpy(spot_dst + spot_count * sizeof(GpuSpotLight), &gpu, sizeof(GpuSpotLight));
        ++spot_count;
    }

}

void VulkanContext::update_fog_volume_ssbo(uint32_t frame_index) {
    FJELL_PROFILE_SCOPE_N("update_fog_volume_ssbo");
    if (current_environment_ == nullptr) return;

    const auto& volumes = current_environment_->fog_volumes;
    auto* dst = reinterpret_cast<GpuFogVolume*>(gpu_device().mapped(gpu_.fog_volume_ssbo(frame_index)).data());

    const uint32_t count = std::min(static_cast<uint32_t>(volumes.size()), MAX_FOG_VOLUMES);
    for (uint32_t i = 0; i < count; ++i) {
        const auto& v = volumes[i];
        GpuFogVolume gpu{};
        // The shader takes a world position into the volume's own space, where
        // the shape is the unit box or sphere, so the inverse is what it needs.
        gpu.inv_transform = glm::inverse(v.transform);
        gpu.shape_falloff = glm::vec4(static_cast<float>(v.shape), v.edge_falloff,
                                      v.motion_response, 0.0f);
        // Already integrated by EnvironmentSubsystem::advance_fog_volumes, so
        // that drift stays continuous while the wind turns it.
        gpu.drift = glm::vec4(v.drift, 0.0f);
        dst[i] = gpu;
    }
}

void VulkanContext::init_game_ui(GameUi& ui, gpu::Format output_format, bool world) {
    const PassResources res = gpu_.pass_resources();
    ui.create(res, *font_library_, gpu_.pipeline_cache(), output_format);
    if (world) ui.create_world_pipelines(res);
}

void VulkanContext::record_game_ui_offscreen(GameUi& ui, OffscreenPass& target,
                                              uint32_t frame_index) {
    if (!ui.loaded()) return;
    record_game_ui_offscreen_multi(
        [&ui](gpu::RenderEncoder& pass, gpu::BindGroup globals, gpu::BindGroup bindless,
              glm::uvec2 extent, uint32_t idx) { ui.record(pass, globals, bindless, extent, idx); },
        target, frame_index);
}

void VulkanContext::record_game_ui_offscreen_multi(const GameUiRecordFn& record_fn,
                                                    OffscreenPass& target,
                                                    uint32_t frame_index) {
    record_game_ui_into(*pre_list_, record_fn, target, frame_index);
}

void VulkanContext::sync_scene_particles(Scene& scene) {
    if (particle_system_) particle_system_->sync_scene(scene, resource_manager_, project_);
}

void VulkanContext::release_scene_particles(const Scene& scene) {
    if (particle_system_) particle_system_->release_scene(scene.id());
}

void VulkanContext::advance_scene_particles(const Scene& scene, float dt) {
    if (particle_system_) particle_system_->advance(scene.id(), dt);
}

void VulkanContext::render_game_ui_preview(GameUi& ui, OffscreenPass& target) {
    FJELL_PROFILE_SCOPE_N("render_game_ui_preview");
    if (!ui.loaded()) return;

    auto vk = gpu_.device();
    VkCommandBuffer cmd{};
    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = gpu_.command_pool();
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;
    vk_check(vkAllocateCommandBuffers(vk, &alloc_info, &cmd), "allocate UI preview command buffer");

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(cmd, &begin), "begin UI preview command buffer");

    {
        gpu::vulkan::CommandBufferList list(gpu_device(), cmd);
        record_game_ui_into(
            list.list(),
            [&ui](gpu::RenderEncoder& pass, gpu::BindGroup globals, gpu::BindGroup bindless,
                  glm::uvec2 extent, uint32_t idx) { ui.record(pass, globals, bindless, extent, idx); },
            target, current_frame_);
    }

    vk_check(vkEndCommandBuffer(cmd), "end UI preview command buffer");

    // The UI may sample textures still sitting in the upload batch; the
    // frame submit waits for those on its timeline, this one-shot does not.
    gpu_.upload_context().wait_all();

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vk_check(vkQueueSubmit(gpu_.graphics_queue(), 1, &submit, VK_NULL_HANDLE), "submit UI preview");
    vk_check(vkQueueWaitIdle(gpu_.graphics_queue()), "wait for UI preview");

    vkFreeCommandBuffers(vk, gpu_.command_pool(), 1, &cmd);
}

void VulkanContext::record_game_ui_into(gpu::CommandList& cmd, const GameUiRecordFn& record_fn,
                                        OffscreenPass& target, uint32_t frame_index) {
    const glm::uvec2 extent = target.extent();
    const bool msaa = target.samples() != gpu::Samples::x1;

    // Whatever the target held is cleared: nothing of it is kept.
    cmd.barrier(target.msaa_color_texture(), {}, gpu::Access::color_attachment);
    if (msaa) cmd.barrier(target.color_texture(), {}, gpu::Access::color_attachment);

    // .fjsl materials sample TIME from the global UBO, but that UBO is
    // only otherwise refreshed by a scene viewport's render request
    // (update_uniform_buffer()) — when this offscreen target is the
    // only thing rendering this frame (e.g. the UI editor tab open with
    // no 3D viewport active), TIME would sit frozen at whatever a scene
    // viewport last wrote, so animated UI materials appear static in
    // the preview. Patch just the time field in place — cheap, and
    // harmless even when a scene request did already refresh the full
    // UBO this frame (same value either way).
    float live_time = elapsed_time_;
    std::memcpy(gpu_device().mapped(gpu_.uniform_buffer(frame_index)).data() + offsetof(GlobalUbo, time),
                &live_time, sizeof(live_time));
    gpu_.bindless_textures().flush(frame_index);
    {
        auto pass = cmd.render({.color = {{.view = target.msaa_color_texture(),
                                           .load = gpu::Load::clear,
                                           .clear = gpu::Clear{0.0f, 0.0f, 0.0f, 0.0f},
                                           .resolve = msaa ? gpu::TextureView(target.color_texture())
                                                           : gpu::TextureView{}}}});
        record_fn(pass, gpu_.global_bindings().group(frame_index), gpu_.bindless_textures().group(frame_index),
                  extent, frame_index);
    }

    // ImGui shows it.
    cmd.barrier(target.color_texture(), gpu::Access::color_attachment, gpu::Access::sampled_fragment);
}

gpu::Format VulkanContext::swapchain_format() const {
    return gpu_.swapchain().format();
}

void VulkanContext::submit_and_present() {
    if (!has_recorded_frame_) return;
    has_recorded_frame_ = false;
    gpu::Frame& frame = *recorded_gpu_frame_;

    // Without async work the frame is its pre list alone. With it, three
    // lists chained across the queues: the compute list runs once pre is
    // done (depth ready for GTAO, the light cull's inputs), and post, which
    // holds the swapchain image and ImGui, once the compute list is; every
    // hand-off the frame graph makes between the queues relies on these
    // waits.
    frame.submit(*pre_list_);
    if (recorded_async_this_frame_) {
        frame.submit(*async_list_, {.after = pre_list_});
        frame.submit(*post_list_, {.after = async_list_});
    }
    recorded_gpu_frame_ = nullptr;
    frame_cmd_ = async_cmd_ = post_cmd_ = VK_NULL_HANDLE;
    presented_texture_ = recorded_image_.texture;
    if (auto shown = gpu_.swapchain().present(frame, recorded_image_); !shown) {
        FJELL_GFX_CRITICAL("{}", shown.error());
    }
}

bool VulkanContext::begin_frame(ThreadPool& pool) {
    FJELL_PROFILE_SCOPE_N("begin_frame");

    // Clean slate for async tracking. render_scene_to sets this when
    // any viewport actually records into the async CB.
    recorded_async_this_frame_ = false;

    // The device waits until the GPU has finished the frame that last used
    // this slot, frees the slot's lists and one-frame memory, and destroys
    // what the frames it has finished used: at least that frame, often later
    // ones too. Transients those frames used are free again.
    frame_ = &gpu_device().begin_frame();
    current_frame_ = frame_->slot();
    frame_serial_ = frame_->serial();
    render_pipeline_.begin_frame();
    gpu_.transient_pool().begin_frame(frame_serial_, gpu_device().finished_frame());
    apply_pending_ddgi_toggle();
    apply_pending_ddgi_probe_count();

    // A font whose recipe changed is rebuilt here, where the retired texture
    // lands in a slot the GPU is done with.
    font_library_->poll();

    if (accel_manager_) accel_manager_->begin_frame(frame_serial_);

    // Lazy-init per-thread command pools (needs thread count from pool)
    gpu_.ensure_secondary_pools(pool.thread_count());
    if (pool.thread_count() > 0) {
        gpu_.secondary_pools().reset_frame(current_frame_);
    }

    // The frame's first graphics list waits for the image where it writes
    // colour. Without one (the window changed size, and the swapchain was
    // remade) the frame ends empty, so the frames after it do not wait for
    // it.
    auto image = gpu_.swapchain().acquire(*frame_);
    if (!image) {
        (void)gpu_device().end_frame(*frame_);
        frame_ = nullptr;
        return false;
    }
    acquired_image_ = *image;
    return true;
}

void VulkanContext::record_frame(const std::vector<ViewportRenderRequest>& viewports,
                                 const EnvironmentState& environment,
                                 Scene* scene, float dt,
                                 uint32_t selected_id, ThreadPool& pool) {
    FJELL_PROFILE_SCOPE_N("record_frame");

    gpu_.render_stats().reset();
    // Always rebuild when there's a scene: update() is what clears the vertex
    // and command buffers, so skipping it on the frame a source turns off
    // leaves that frame's geometry on screen until something else redraws.
    if (scene) {
        debug_draw_->update(*scene, selected_id, show_colliders_, debug_draw_queue_);
    }
    if (debug_draw_queue_) {
        debug_draw_queue_->tick(dt);
    }

    // Sync render subsystems — may rebuild GPU buffers.
    // Safe here: begin_frame() already waited on this frame slot's fence,
    // so the GPU is done with this slot's resources.
    {
        RenderSubsystemContext sub_ctx{};
        sub_ctx.scene = scene;
        sub_ctx.resource_manager = resource_manager_;
        sub_ctx.project = project_;
        sub_ctx.frame_index = current_frame_;
        for (auto& sys : render_subsystems_) sys->sync(sub_ctx);
    }


    // Update scene SDF — register mesh SDFs, auto-fit volume, hand every
    // placed mesh to the pass so it can rebuild only what moved
    {
        AABB scene_bounds;
        scene_sdf_pass().begin_gather();
        // Re-accumulated from the terrains actually present below, so removing
        // one does not leave the ray bias set for geometry that is gone. Both
        // tracers read it: they trace the same acceleration structure, so they
        // meet the same coarse terrain.
        rt_reflection_pass().set_trace_cell_size(0.0F);
        ss_gi_pass().set_trace_cell_size(0.0F);

        auto gather_sdf = [&](Scene& s) {
            // Terrain has no Model, so it is not among the meshes below, but
            // it is the largest thing in most scenes. Leaving it out sizes the
            // SDF volume and the DDGI probe grid as though the ground were not
            // there.
            for (auto [obj, tc] : s.view<TerrainComponent>()) {
                auto* spatial = obj->as_spatial();
                if (!spatial) continue;
                if (tc->terrain != nullptr) {
                    TerrainAsset& terrain = *tc->terrain;

                    // Terrain is ray-traced against a coarser mesh than it is
                    // drawn from, so a ray spawned on the drawn surface can
                    // start inside the traced one. Both tracers offset their
                    // rays by how far the two can drift, which is set by the
                    // coarse mesh's spacing; the coarsest terrain in the scene
                    // decides it.
                    const float ray_spacing = terrain.ray_mesh_spacing();
                    rt_reflection_pass().set_trace_cell_size(
                        std::max(rt_reflection_pass().trace_cell_size(), ray_spacing));
                    ss_gi_pass().set_trace_cell_size(
                        std::max(ss_gi_pass().trace_cell_size(), ray_spacing));

                    const glm::vec3 origin = terrain.world_min_corner(spatial->world_matrix_);
                    const float span = terrain.world_size();
                    AABB terrain_bounds{
                        {origin.x, origin.y + terrain.min_world_height(), origin.z},
                        {origin.x + span, origin.y + terrain.max_world_height(), origin.z + span}};
                    scene_bounds.expand(terrain_bounds.min);
                    scene_bounds.expand(terrain_bounds.max);

                    // The terrain is one instance whose grid holds its heights.
                    // It never moves on its own, so after the first build it
                    // costs nothing until a sculpt stroke, which re-copies the
                    // touched samples and dirties only their box.
                    const auto* entry = scene_sdf_pass().register_terrain_sdf(terrain);
                    if (entry != nullptr) {
                        if (entry->generation != terrain.edit_generation()) {
                            const auto rect = terrain.edits_since(entry->generation);
                            if (!rect.empty()) {
                                scene_sdf_pass().update_terrain_sdf(terrain, rect.x, rect.z,
                                                                    rect.width, rect.height);
                                const float spacing =
                                    span / static_cast<float>(terrain.sample_count() - 1);
                                AABB edited{
                                    {origin.x + static_cast<float>(rect.x) * spacing,
                                     terrain_bounds.min.y,
                                     origin.z + static_cast<float>(rect.z) * spacing},
                                    {origin.x + static_cast<float>(rect.x + rect.width) * spacing,
                                     terrain_bounds.max.y,
                                     origin.z + static_cast<float>(rect.z + rect.height) * spacing}};
                                scene_sdf_pass().invalidate_region(edited);
                            }
                        }

                        GpuSdfInstance inst{};
                        inst.kind = SdfInstanceKind::terrain;
                        inst.grid_offset = entry->offset;
                        inst.grid_resolution = glm::uvec3(entry->samples, entry->samples, 1);
                        inst.terrain_origin = {origin.x, origin.z};
                        inst.terrain_size = span;
                        inst.terrain_height_scale = terrain.height_scale();
                        inst.terrain_height_offset = terrain.height_offset() + origin.y;
                        // Rays cannot run the layer blend, so an SDF-traced
                        // probe ray bounces the terrain's first layer, as the
                        // ray-traced tracer does.
                        const uint32_t ground = terrain.layer_material_id(0);
                        scene_sdf_pass().gather_instance(
                            SceneSdfPass::instance_key(s.id(), obj->id), terrain_bounds,
                            spatial->world_dirty_, inst, std::span<const uint32_t>(&ground, 1));
                    }
                }
            }

            for (auto [obj, mesh_comp] : s.view<MeshComponent>()) {
                auto* spatial = obj->as_spatial();
                if (!spatial) continue;
                if (!mesh_comp->model) continue;

                auto* model = mesh_comp->model;

                // Expand scene bounds from all mesh objects (not just SDF ones)
                auto world_aabb = model->local_aabb().transform(spatial->world_matrix_);
                scene_bounds.expand(world_aabb.min);
                scene_bounds.expand(world_aabb.max);

                if (!model->has_sdf()) continue;

                scene_sdf_pass().register_model_sdf(*model, reinterpret_cast<uintptr_t>(model));

                auto it = scene_sdf_pass().model_entry(reinterpret_cast<uintptr_t>(model));
                if (it) {
                    GpuSdfInstance inst{};
                    inst.world_to_sdf = glm::inverse(spatial->world_matrix_);
                    inst.sdf_min = it->sdf_min;
                    inst.sdf_scale = it->max_dist;
                    inst.sdf_max = it->sdf_max;
                    inst.grid_offset = it->offset;
                    inst.grid_resolution = glm::uvec3(it->resolution);
                    inst.mesh_min = it->mesh_min;
                    inst.mesh_max = it->mesh_max;

                    // The materials it draws with, whose colours an
                    // SDF-traced probe ray bounces off it.
                    std::array<uint32_t, SDF_SLOT_MATERIALS> materials{};
                    size_t material_count = 0;
                    if (mesh_comp->slot_materials.empty()) {
                        materials[material_count++] = mesh_comp->resolved_material_id();
                    }
                    for (uint32_t slot = 0; slot < mesh_comp->slot_materials.size()
                                            && material_count < materials.size(); ++slot) {
                        materials[material_count++] = mesh_comp->resolved_material_id(slot);
                    }
                    scene_sdf_pass().gather_instance(
                        SceneSdfPass::instance_key(s.id(), obj->id),
                        world_aabb, spatial->world_dirty_, inst,
                        std::span<const uint32_t>(materials.data(), material_count));
                }
            }
        };

        if (scene) gather_sdf(*scene);

        // Gather from active editor context's preview scene (e.g. VFX editor ground plane)
        if (active_render_scene_ && active_render_scene_ != scene) {
            gather_sdf(*active_render_scene_);
        }

        // Auto-fit SDF volume and DDGI probe grid to scene bounds.
        // Snap to integers to avoid per-frame floating-point jitter.
        if (scene_bounds.valid()) {
            constexpr float SDF_PADDING = 3.0f;
            glm::vec3 padded_min = glm::floor(scene_bounds.min - glm::vec3(SDF_PADDING));
            glm::vec3 padded_max = glm::ceil(scene_bounds.max + glm::vec3(SDF_PADDING));

            // Enforce minimum volume size so sparse scenes still work
            glm::vec3 extent = padded_max - padded_min;
            glm::vec3 min_extent(10.0f);
            if (glm::any(glm::lessThan(extent, min_extent))) {
                glm::vec3 center = (padded_min + padded_max) * 0.5f;
                extent = glm::max(extent, min_extent);
                padded_min = center - extent * 0.5f;
                padded_max = center + extent * 0.5f;
            }

            scene_sdf_pass().set_bounds(padded_min, padded_max);

            // Update particle collision SDF bounds
            if (particle_system_) {
                particle_system_->set_sdf_bounds(padded_min, padded_max);
            }

            // Fit the DDGI probe grid to scene bounds, and re-fit when the
            // scene grows out of it.
            //
            // A probe's irradiance is only meaningful for the world position
            // it was traced at, so moving the grid discards the whole atlas.
            // That is why this cannot simply track the bounds every frame:
            // an object drifting a metre would re-fit continuously and the
            // probes would never converge. It only re-fits when content has
            // actually escaped what the grid covers, which is the case the
            // frozen grid got wrong — anything outside clamps to an edge
            // probe and picks up lighting locked to a world position rather
            // than to the geometry.
            auto& ddgi_cfg = ddgi_pass().config();

            glm::vec3 spacing = extent / glm::vec3(ddgi_cfg.probe_counts - glm::ivec3(1));

            if (!ddgi_grid_initialized_) {
                ddgi_pass().refit_grid(padded_min, spacing);
                ddgi_grid_initialized_ = true;
                ddgi_grid_min_ = padded_min;
                ddgi_grid_max_ = padded_min + glm::vec3(ddgi_cfg.probe_counts - glm::ivec3(1)) * spacing;
            } else {
                // Re-fit only once the scene reaches outside the grid. The
                // margin is a fraction of a cell so a probe still brackets
                // the content rather than sitting exactly on it, and it
                // keeps a boundary-hugging object from toggling the refit
                // on and off frame to frame.
                glm::vec3 margin = ddgi_cfg.probe_spacing * 0.5f;
                bool escaped = glm::any(glm::lessThan(padded_min, ddgi_grid_min_ - margin))
                            || glm::any(glm::greaterThan(padded_max, ddgi_grid_max_ + margin));
                if (escaped) {
                    ddgi_pass().refit_grid(padded_min, spacing);
                    ddgi_grid_min_ = padded_min;
                    ddgi_grid_max_ = padded_min
                        + glm::vec3(ddgi_cfg.probe_counts - glm::ivec3(1)) * spacing;
                    FJELL_GFX_DEBUG("DDGI grid re-fitted to ({:.1f},{:.1f},{:.1f})..({:.1f},{:.1f},{:.1f})",
                                    padded_min.x, padded_min.y, padded_min.z,
                                    ddgi_grid_max_.x, ddgi_grid_max_.y, ddgi_grid_max_.z);
                }
            }
        }

        scene_sdf_pass().end_gather(current_frame_);
    }

    // Update per-frame global bindings before recording
    if (!viewports.empty() && viewports[0].target) {
        auto& gb = gpu_.global_bindings();

        gb.set(current_frame_, "depth_texture",
               gpu::sampled(viewports[0].target->depth_texture(), gpu_.depth_sampler()));

        const gpu::Texture ao =
            default_pp_->gtao() ? default_pp_->gtao()->ao : gtao_pass().ao_texture();
        gb.set(current_frame_, "ao_texture", gpu::sampled(ao, gtao_pass().ao_sampler()));

        gb.set(current_frame_, "screen_color",
               gpu::sampled(viewports[0].target->screen_color_texture(), post_pass().output_sampler()));

        // GI indirect: DDGI handles sun bounce indirect.
        //
        // The enabled check matters as much as the null check. A disabled
        // pass stops writing but its image keeps whatever it last produced,
        // and binding that composites a frozen frame of indirect light over
        // everything — a ghost that follows the camera because it is screen
        // space. Falling back to the black fallback image is what actually
        // turns the contribution off.
        gpu::Texture gi = gpu_.gi_fallback();
        bound_gi_indirect_ = {};
        auto* ddgi_imgs = default_pp_->ddgi();
        if (ddgi_pass().config().enabled && ddgi_imgs && ddgi_imgs->indirect_written) {
            gi = ddgi_imgs->indirect;
            bound_gi_indirect_ = ddgi_imgs->indirect;
        }
        gb.set(current_frame_, "gi_indirect", gpu::sampled(gi, gpu_.gi_fallback_sampler()));

        // SS-GI emissive: screen-space emissive color bleeding (supplements DDGI)
        gpu::Texture ss_gi = gpu_.gi_fallback();
        bound_ss_gi_emissive_ = {};
        auto* ss_gi_imgs = default_pp_->ss_gi();
        // Same ghost as DDGI above: a disabled pass keeps its last output.
        if (ss_gi_pass().config().enabled && ss_gi_imgs && ss_gi_imgs->written) {
            ss_gi = ss_gi_imgs->indirect;
            bound_ss_gi_emissive_ = ss_gi_imgs->indirect;
        }
        gb.set(current_frame_, "ss_gi_emissive", gpu::sampled(ss_gi, gpu_.gi_fallback_sampler()));

        gb.set(current_frame_, "contact_shadow",
               gpu::sampled(contact_shadow_pass().shadow(), contact_shadow_pass().shadow_sampler()));
    }

    // The frame's lists: pre, and, where there is a compute queue, the
    // compute list and the graphics list after it that async passes need.
    // They stay empty on frames without compute work, and are then never
    // submitted.
    pre_list_ = &frame_->commands(gpu::Queue::graphics);
    async_list_ = nullptr;
    post_list_ = nullptr;
    if (gpu_device().caps().async_compute) {
        async_list_ = &frame_->commands(gpu::Queue::compute);
        post_list_ = &frame_->commands(gpu::Queue::graphics);
    }
    frame_cmd_ = gpu::vulkan::native_command_buffer(*pre_list_);
    async_cmd_ = async_list_ != nullptr ? gpu::vulkan::native_command_buffer(*async_list_)
                                        : VK_NULL_HANDLE;
    post_cmd_ = post_list_ != nullptr ? gpu::vulkan::native_command_buffer(*post_list_)
                                      : VK_NULL_HANDLE;
    record_command_buffer(frame_cmd_, acquired_image_, viewports, environment, scene, dt,
                          selected_id, pool);

    recorded_gpu_frame_ = frame_;
    frame_ = nullptr;
    recorded_image_ = acquired_image_;
    has_recorded_frame_ = true;
}

void VulkanContext::wait_idle() {
    vkDeviceWaitIdle(gpu_.device());
}

void VulkanContext::apply_pending_ddgi_toggle() {
    if (!has_pending_ddgi_toggle_) return;
    has_pending_ddgi_toggle_ = false;

    auto& config = const_cast<PostProcessConfig&>(default_pp_->config());
    if (config.ddgi == pending_ddgi_toggle_) return;

    // Wait for ALL GPU work to complete before touching shared resources
    submit_and_present(); // flush any pending recorded frame
    vkDeviceWaitIdle(gpu_.device());

    config.ddgi = pending_ddgi_toggle_;

    // Get current viewport size
    uint32_t w = 256, h = 256;
    auto* tm = default_pp_->post();
    if (tm && tm->extent.x > 0) {
        w = tm->extent.x;
        h = tm->extent.y;
    }

    // Resize triggers create/destroy of DDGI images based on updated config
    default_pp_->resize(w, h);
}

void VulkanContext::rebuild_ddgi_resources() {
    uint32_t w = 256, h = 256;
    auto* tm = default_pp_->post();
    if (tm && tm->extent.x > 0) {
        w = tm->extent.x;
        h = tm->extent.y;
    }
    // Remade outright: resize() keeps the probe field when the probe count
    // is unchanged, and this rebuild must happen even then. The frames
    // still using the old one keep it until they are done.
    if (auto* imgs = default_pp_->ddgi()) {
        imgs->remake(gpu_device(), ddgi_pass().config(), w, h);
    } else {
        default_pp_->resize(w, h);
    }

    // The fresh atlases carry no lighting, so re-fit and re-seed the grid
    // against the current scene rather than resuming the old fit.
    ddgi_grid_initialized_ = false;
}

void VulkanContext::apply_pending_ddgi_probe_count() {
    if (!has_pending_ddgi_probe_count_) return;
    has_pending_ddgi_probe_count_ = false;

    // Only the default viewport is rebuilt here. Offscreen viewports own
    // their own PostProcessResources and pick the new count up through
    // SceneViewport::resize().
    if (default_pp_ == nullptr) return;
    if (!default_pp_->config().ddgi) return;

    rebuild_ddgi_resources();
}

void VulkanContext::reload_shaders(const std::vector<std::string>& changed_paths) {
    auto* cache = gpu_.pipeline_cache();
    if (cache == nullptr) return;
    if (cache->reload_changed(changed_paths) == 0) return;
    // Depth variants are built from the same entries, so they follow whatever
    // was rebuilt. The particle system reads raw stages rather than FJSL and
    // is untouched by a .fjsl edit.
    sync_custom_depth_pipelines();
}

void VulkanContext::reload_pipeline() {
    gpu_.pipeline_cache()->recreate_all();
    particle_system_->recreate_pipelines();
    sync_custom_depth_pipelines();
}

void VulkanContext::sync_custom_depth_pipelines() {
    auto* pc = gpu_.pipeline_cache();
    if (!pc) return;

    // Registered afresh, so a shader that no longer displaces its vertices
    // falls back to the pass's own pipeline.
    auto* msd = passes_.try_get<MeshShaderDepthPass>("mesh_shader_depth");
    if (msd) msd->clear_custom_depth();
    depth_prepass().clear_custom_depth();
    shadow_pass().clear_custom_shadows();
    for (uint32_t id = 0; id < pc->shader_count(); ++id) {
        if (!pc->has_custom_vertex(id)) continue;

        const auto& depth_mesh = pc->depth_mesh_spv(id);
        if (msd && !depth_mesh.empty()) {
            msd->register_custom_depth(id, depth_mesh, pc->cull(id));
        }

        const auto& depth_vert = pc->depth_vert_spv(id);
        if (!depth_vert.empty()) {
            depth_prepass().register_custom_depth(id, depth_vert, pc->cull(id));
        }

        const auto& shadow_vert = pc->shadow_vert_spv(id);
        if (!shadow_vert.empty()) {
            shadow_pass().register_custom_shadow(id, shadow_vert);
        }
    }
}

// ── Scene Viewports ──────────────────────────────────────────────────────

uint32_t VulkanContext::create_scene_viewport(const SceneViewportConfig& config) {
    // A name already taken is replaced, and the old viewport goes the way
    // destroy takes it: its images outlive the frames sampling them, and its
    // table entry is emptied.
    if (scene_viewports_.contains(config.name)) destroy_scene_viewport(config.name);

    uint32_t bindless_idx =
        gpu_.bindless_textures().reserve_index(gpu_device().sampler({.address = gpu::Address::clamp}));

    const PostProcessResources::PassRefs refs{ddgi_pass(), ss_gi_pass()};

    auto viewport = std::make_unique<SceneViewport>(gpu_device(), config, refs, bindless_idx);
    // A global set of its own, so its view-dependent images never replace
    // the frame's.
    viewport->set_global_group(gpu_.global_bindings().make_view_group(config.name));

    scene_viewports_[config.name] = std::move(viewport);
    return bindless_idx;
}

void VulkanContext::destroy_scene_viewport(const std::string& name) {
    auto it = scene_viewports_.find(name);
    if (it == scene_viewports_.end()) return;
    wait_idle();
    gpu_.bindless_textures().update_entry(it->second->bindless_index(), {});
    scene_viewports_.erase(it);
}

SceneViewport* VulkanContext::scene_viewport(const std::string& name) {
    auto it = scene_viewports_.find(name);
    return it != scene_viewports_.end() ? it->second.get() : nullptr;
}

std::vector<std::string> VulkanContext::scene_viewport_names() const {
    std::vector<std::string> names;
    names.reserve(scene_viewports_.size());
    for (const auto& [name, viewport] : scene_viewports_) {
        names.push_back(name);
    }
    return names;
}

uint32_t VulkanContext::scene_viewport_bindless_index(const std::string& name) const {
    auto it = scene_viewports_.find(name);
    return it != scene_viewports_.end() ? it->second->bindless_index() : 0;
}

void VulkanContext::render_scene_viewports(Scene* scene, float dt, uint32_t selected_id,
                                            const EnvironmentState& environment, ThreadPool& pool) {
    for (auto& [name, viewport] : scene_viewports_) {
        ViewportRenderRequest req = viewport->build_request();
        req.global_group = viewport->global_group();
        render_preview(req, environment, scene, dt, selected_id, pool);
        viewport->sync_bindless(gpu_.bindless_textures());
    }
}

} // namespace fjell
