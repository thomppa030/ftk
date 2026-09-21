#include "renderer/vulkan_context.hpp"
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
#include "core/window.hpp"
#include "scene/scene.hpp"
#include "scene/light_node.hpp"

#include <imgui.h>
#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace fjell {

VulkanContext::VulkanContext(Window& window)
    : gpu_{window}
    , target_pool_{gpu_.device(), gpu_.allocator()} {
    FJELL_GFX_INFO("Initializing Vulkan context");

    // Finalize global descriptor layout/pool/sets (builtins already declared
    // by GpuContext constructor; additional bindings could be declared here)
    gpu_.finalize_descriptors();

    // Scene gathering: indirect batches + draw data descriptors
    gatherer_.init(gpu_);

    // Init pipelines now that draw_data_layout is available
    gpu_.init_pipelines(gatherer_.draw_data_descriptor().layout());

    // Build PassResources with draw_data_layout
    PassResources pass_resources = gpu_.pass_resources();
    pass_resources.draw_data_layout = gatherer_.draw_data_descriptor().layout();

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

    // Create acceleration structure manager for DDGI RT backend
    if (gpu_.device_ref().ray_tracing_supported()) {
        accel_manager_ = std::make_unique<AccelStructureManager>(
            gpu_.device_ref(), gpu_.allocator(),
            gpu_.command_pool(), gpu_.graphics_queue(),
            gpu_.upload_context(), gpu_.deferred_deleter());
        accel_manager_->create_particle_tlas_pipeline(gpu_.vk_pipeline_cache());
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
        ddgi_pass().set_particle_system(particle_system_);
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
    debug_draw_ = std::make_unique<DebugDraw>(gpu_.device(), gpu_.allocator());

    // The engine's own font is the one every UI falls back to.
    font_library_ = std::make_unique<FontLibrary>();
    font_library_->create(
        FontAtlasGpu{gpu_.device(), gpu_.allocator(), gpu_.device_ref().physical_device(),
                     &gpu_.upload_context(), &gpu_.bindless_textures(), &gpu_.deferred_deleter()},
        base_path::resolve("engine_assets/fonts/Inter.fjfont"));

    // Default post-process resources for the main viewport (all passes enabled)
    PostProcessConfig default_config{};
    default_config.ssr = true;
    default_config.volumetric_fog = true;
    default_config.ddgi = true;
    default_config.ss_gi = true;
    default_config.taa = true;
    default_config.rt_reflection = gpu_.device_ref().ray_tracing_supported();
    PostProcessResources::PassRefs refs{hiz_pass(), gtao_pass(), volumetric_fog_pass(), tonemap_pass(), ddgi_pass(), ss_gi_pass(), taa_resolve_pass(), rt_reflection_pass()};
    default_pp_ = std::make_unique<PostProcessResources>(gpu_.device(), gpu_.allocator(), 256, 256, default_config, refs);

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

void VulkanContext::set_render_settings(const RenderSettings& s) {
    // Quality
    if (auto* cp = cull_pass()) {
        cp->set_enabled(s.gpu_cull);
    }
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
    if (!ddgi_settings_seen_) {
        ddgi_settings_seen_ = true;
        request_ddgi_settle_rebuild();
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

    // Tonemap
    tonemap_pass().set_operator(static_cast<TonemapPass::Operator>(s.tonemap.op));
    tonemap_pass().set_auto_exposure(s.tonemap.auto_exposure);
    tonemap_pass().set_exposure(s.tonemap.exposure_ev);
    tonemap_pass().set_adaptation_speed(s.tonemap.adaptation_speed);

    // Frame graph
    render_pipeline_.set_aliasing_enabled(s.frame_graph_aliasing);
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
           a.queue      == b.queue &&
           a.phase      == b.phase &&
           a.sub_order  == b.sub_order &&
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

FrameContext VulkanContext::build_frame_context(VkCommandBuffer cmd, OffscreenPass& target,
                                                 Scene* scene, float dt, uint32_t selected_id,
                                                 PostProcessResources* pp, bool skip_shadows,
                                                 bool flip_tonemap_y,
                                                 const FjGraph* viewport_graph,
                                                 VkDescriptorSet global_set,
                                                 const SkyBackdrop* backdrop) {
    FrameContext ctx{};
    ctx.cmd = cmd;
    // Async compute CB is set on the main-viewport path in render_frame
    // (after build_frame_context returns). Preview viewports never hit
    // the async path — they submit their own single-CB bundle.
    ctx.target = &target;
    ctx.queue = &gatherer_.render_queue();
    ctx.camera_position = gatherer_.camera_position();
    // Previews bind their own private global set; everyone else the per-frame set.
    ctx.global_descriptor_set = global_set != VK_NULL_HANDLE
        ? global_set : gpu_.global_descriptor_set(current_frame_);
    ctx.bindless_set = gpu_.bindless_textures().descriptor_set();
    ctx.draw_data_set = gatherer_.draw_data_descriptor().descriptor_set(current_frame_);
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
    if (passes_.has("meshlet_cull") && runs_pass(viewport_graph, "meshlet_cull")) {
        ctx.meshlet_set = gpu_.meshlet_descriptor().descriptor_set(current_frame_);
        ctx.gpu_driven_indirect = meshlet_instance_pass()->indirect_buffer(current_frame_);
        ctx.gpu_driven_segment_size = meshlet_instance_pass()->segment_size(current_frame_);
    }
    ctx.pp_resources = pp;
    ctx.descriptor_cache = &gpu_.descriptor_cache();
    ctx.transient_pool = &gpu_.transient_pool();
    ctx.registry = &resource_registry_;
    ctx.skip_shadows = skip_shadows;
    ctx.flip_tonemap_y = flip_tonemap_y;
    ctx.backdrop = backdrop;
    ctx.target_pool = &target_pool_;

    // Read inv_view_proj from the already-written GlobalUbo
    auto* ubo_data = static_cast<const GlobalUbo*>(gpu_.uniform_buffer(current_frame_).mapped());
    ctx.inv_view_proj = ubo_data->inv_view_proj;

    // Extended context for self-registering passes
    ctx.render_stats = &gpu_.render_stats();
#ifdef FJELL_ENABLE_TRACY
    ctx.tracy_ctx = gpu_.tracy_ctx();
#endif
    ctx.draw_mesh_tasks_indirect_fn = gpu_.device_ref().draw_mesh_tasks_indirect_fn();
    ctx.subsystems = subsystem_ptrs_.data();
    ctx.subsystem_count = static_cast<uint32_t>(subsystem_ptrs_.size());
    ctx.active_sources = active_sources_;
    ctx.passes = &passes_;
    ctx.playing = playing_;
    ctx.world_ui_callback = world_ui_callback_;

    // Shadow cascade batches (filled from gatherer, consumed by ShadowPass::build)
    for (uint32_t c = 0; c < FrameContext::MAX_SHADOW_CASCADES; ++c) {
        ctx.shadow_cascade_batches[c] = gatherer_.shadow_batch(c);
        ctx.shadow_cascade_draw_data[c] =
            gatherer_.shadow_draw_data(c).descriptor_set(current_frame_);
        ctx.shadow_cascade_commands[c] = &gatherer_.shadow_cmds(c);
    }

    return ctx;
}

void VulkanContext::render_scene_to(VkCommandBuffer cmd, OffscreenPass& target,
                                    Scene* scene, float dt, uint32_t selected_id,
                                    ThreadPool& pool, PostProcessResources* pp,
                                    bool skip_shadows, bool flip_tonemap_y,
                                    const FjGraph* viewport_graph,
                                    VkDescriptorSet global_set,
                                    const SkyBackdrop* backdrop) {
    FJELL_PROFILE_SCOPE_N("render_scene_to");

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
        if (ddgi_pass().backend() == DdgiBackend::ray_traced) {
            gi.gi_reach = ddgi_pass().trace_reach_bounds();
        }
        gatherer_.gather(gi);
    }

    // Flush any pending bindless texture/material updates
    gpu_.bindless_textures().update_descriptors();
    gpu_.bindless_textures().update_material_buffer();

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

    current_ctx_ = build_frame_context(cmd, target, scene, dt, selected_id,
                                        pp, skip_shadows, flip_tonemap_y,
                                        viewport_graph, global_set, backdrop);
    // Hand the async CB + post-compute graphics CB to the graph, but
    // only for the main frame CB — preview viewports get their own
    // one-shot CBs from render_preview and have no paired compute/post
    // CBs, so routing their passes through the main frame's async CB
    // would interleave preview compute dispatches into a CB that
    // submit_and_present fences independently. Nulling both handles
    // folds previews back to single-CB behavior.
    const bool is_main_cb = (cmd == gpu_.command_buffer(current_frame_));
    current_ctx_.async_compute_cmd = is_main_cb ? gpu_.async_compute_cb(current_frame_)
                                                : VK_NULL_HANDLE;
    current_ctx_.graphics_post_cmd = is_main_cb ? gpu_.graphics_post_cb(current_frame_)
                                                : VK_NULL_HANDLE;


    // Init screen color image on first use (or after resize) so the descriptor
    // at binding 8 doesn't point to an UNDEFINED-layout image.
    if (target.screen_color_needs_init()) {
        vk_utils::transition_image(cmd, target.screen_color_image(),
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        target.mark_screen_color_initialized();
    }

    // First-frame seed for color/resolve_color. Pre-baked descriptors (ImGui
    // texture, post-process inputs) hold this image at SHADER_READ_ONLY and
    // are bound by draws across both graphics_pre and graphics_post when
    // async-compute is on. The framegraph slice tracker resets the image to
    // UNDEFINED at begin_frame; on the very first frame, before any pass has
    // written to it, the image is genuinely UNDEFINED, which trips a draw-
    // time descriptor-vs-layout check (VUID-vkCmdDraw-None-09600). Seeding
    // matches what frame N+1 already inherits from frame N's last access.
    if (target.color_needs_init()) {
        vk_utils::transition_image(cmd, target.color_image(),
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        target.mark_color_initialized();
    }

    // ao_image's initial layout (UNDEFINED → SHADER_READ) is seeded inside
    // AoPass::Images::create via a one-shot graphics-queue submit, so by
    // the time we get here the global ao_texture descriptor's baked layout
    // already matches the image. No per-frame baseline needed.

    // Populate resource registry — passes self-register their outputs,
    // then non-pass resources (meshlet bounds) are added manually.
    resource_registry_.clear();
    passes_.register_resources_all(resource_registry_);
    if (gpu_.meshlet_buffer()) {
        resource_registry_.register_buffer("meshlet_bounds", {
            gpu_.meshlet_buffer()->bounds_buffer(),
            gpu_.meshlet_buffer()->bounds_buffer_size()});
    }

    // FJELL_DISABLE_ASYNC=1 forces every pass back onto the graphics queue.
    // Tracy A/B knob: with vs. without async compute. Re-evaluated per
    // frame because `getenv` is cheap and this is gated by a debug-only
    // workflow.
    static const bool kAsyncDisabled = []() {
        const char* v = std::getenv("FJELL_DISABLE_ASYNC");
        return v != nullptr && v[0] != '0' && v[0] != '\0';
    }();
    const bool async_available = !kAsyncDisabled
                              && gpu_.device_ref().async_compute_supported();
    bool any_async = render_pipeline_.execute(
        cmd, target, target.sample_count(), active_sources_,
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
    if (!req.target) return;
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

    // Ensure AO image is in a valid layout — after resize it may be UNDEFINED.
    // The preview doesn't run GTAO, but the color pass reads ao_texture from
    // the global descriptor set. Transition it here so the read is valid.
    auto* gtao_imgs = default_pp_->gtao();
    if (gtao_imgs && gtao_imgs->images_need_init) {
        VkImageMemoryBarrier2 ao_barrier{};
        ao_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        ao_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ao_barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        ao_barrier.image = gtao_imgs->ao_image;
        ao_barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        ao_barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        ao_barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;

        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &ao_barrier;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    // Update the view-dependent global image bindings to point at THIS
    // viewport's targets. A preview with its own global_set writes into that
    // private set (inheriting the per-frame set's static bindings via a copy)
    // so it never mutates the shared per-frame set the main command buffer
    // binds — rewriting a set an in-flight CB references is a DEVICE_LOST.
    // Without a private set (legacy callers) fall back to the per-frame set.
    {
        auto& gb = gpu_.global_bindings();
        VkImageView ao_view = gtao_imgs ? gtao_imgs->ao_view : gtao_pass().ao_view();

        if (req.global_set != VK_NULL_HANDLE) {
            // Inherit sky/shadow/sdf/ubo/etc. from the (idle) per-frame set,
            // then overwrite only the view-dependent images.
            gb.copy_from_frame(req.global_set, current_frame_);
            gb.write_image_to(req.global_set, "depth_texture",
                req.target->depth_view(), gpu_.depth_sampler(),
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
            gb.write_image_to(req.global_set, "ao_texture",
                ao_view, gtao_pass().ao_sampler(),
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            gb.write_image_to(req.global_set, "screen_color",
                req.target->screen_color_view(), tonemap_pass().output_sampler(),
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            gb.write_image_to(req.global_set, "gi_indirect",
                gpu_.gi_fallback_view(), gpu_.gi_fallback_sampler(),
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            gb.write_image_to(req.global_set, "ss_gi_emissive",
                gpu_.gi_fallback_view(), gpu_.gi_fallback_sampler(),
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            gb.write_image_to(req.global_set, "contact_shadow",
                contact_shadow_pass().shadow_view(),
                contact_shadow_pass().shadow_sampler(),
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        } else {
            gb.write_image(current_frame_, "depth_texture",
                req.target->depth_view(), gpu_.depth_sampler(),
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
            gb.write_image(current_frame_, "ao_texture",
                ao_view, gtao_pass().ao_sampler(),
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            gb.write_image(current_frame_, "screen_color",
                req.target->screen_color_view(), tonemap_pass().output_sampler(),
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            gb.write_image(current_frame_, "gi_indirect",
                gpu_.gi_fallback_view(), gpu_.gi_fallback_sampler(),
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            gb.write_image(current_frame_, "ss_gi_emissive",
                gpu_.gi_fallback_view(), gpu_.gi_fallback_sampler(),
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            gb.write_image(current_frame_, "contact_shadow",
                contact_shadow_pass().shadow_view(),
                contact_shadow_pass().shadow_sampler(),
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        gb.flush_writes();
    }

    update_uniform_buffer(current_frame_, req.view, req.projection,
                          req.camera_position, environment, scene, req.target->extent(), *pp);
    update_light_ssbo(current_frame_, scene);
    update_fog_volume_ssbo(current_frame_);
    render_scene_to(cmd, *req.target, scene, dt, selected_id, pool, pp,
                    req.skip_shadows, req.flip_tonemap_y, req.viewport_graph,
                    req.global_set, req.backdrop);

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
    if (VkResult r = vkQueueWaitIdle(gpu_.graphics_queue()); r != VK_SUCCESS) {
        FJELL_GFX_CRITICAL("render_preview vkQueueWaitIdle failed: VkResult={}", static_cast<int>(r));
        if (r == VK_ERROR_DEVICE_LOST) gpu_.device_ref().dump_device_fault("preview wait idle");
    }

    vkFreeCommandBuffers(vk, gpu_.command_pool(), 1, &cmd);
}

void VulkanContext::record_command_buffer(VkCommandBuffer cmd, uint32_t image_index,
                                          const std::vector<ViewportRenderRequest>& viewports,
                                          const EnvironmentState& environment,
                                          Scene* scene, float dt,
                                          uint32_t selected_id, ThreadPool& pool) {
    FJELL_PROFILE_SCOPE_N("record_command_buffer");
    current_environment_ = &environment;
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    if (vkBeginCommandBuffer(cmd, &begin_info) != VK_SUCCESS) {
        FJELL_GFX_CRITICAL("Failed to begin recording command buffer");
        return;
    }

    // Async compute CB and post-compute graphics CB begin alongside
    // the main graphics CB. They stay empty on frames without compute
    // work — submit_and_present detects that and skips the extra
    // submissions.
    VkCommandBuffer async_cmd = gpu_.async_compute_cb(current_frame_);
    VkCommandBuffer post_cmd = gpu_.graphics_post_cb(current_frame_);
    if (async_cmd != VK_NULL_HANDLE) {
        if (vkBeginCommandBuffer(async_cmd, &begin_info) != VK_SUCCESS) {
            FJELL_GFX_CRITICAL("Failed to begin recording async compute command buffer");
            return;
        }
    }
    if (post_cmd != VK_NULL_HANDLE) {
        if (vkBeginCommandBuffer(post_cmd, &begin_info) != VK_SUCCESS) {
            FJELL_GFX_CRITICAL("Failed to begin recording post-compute graphics command buffer");
            return;
        }
    }

    // Dispatch pending SDF compositing (recorded into the main command buffer, no stall)
    if (scene_sdf_pass().has_pending_update()) {
        scene_sdf_pass().compose_pending(cmd);
    }

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
        prep_ctx.thread_pool = &pool;
        // Terrain uploads its ray mesh here: this is the pass hook that carries
        // the scene, and geometry has to reach the mega-buffer before the TLAS
        // is built during recording.
        prep_ctx.mega_buffer = gpu_.mega_buffer();
        if (!viewports.empty() && viewports[0].target) {
            prep_ctx.view = viewports[0].view;
            prep_ctx.projection = viewports[0].projection;
            prep_ctx.camera_position = viewports[0].camera_position;
            prep_ctx.target = viewports[0].target;
        }
        passes_.prepare_frame_all(prep_ctx, gpu_);
    }

    // IBL intensity from the environment (consumed by update_uniform_buffer)
    ibl_intensity_ = environment.sky.ibl_intensity;
    sky_wants_hdri_ = !environment.sky.hdri_path.empty();

    // --- Render scene once per viewport ---
    for (const auto& vp : viewports) {
        if (!vp.target) continue;
        auto* pp = vp.pp_resources ? vp.pp_resources : default_pp_.get();
        update_uniform_buffer(current_frame_, vp.view, vp.projection,
                              vp.camera_position, environment, scene, vp.target->extent(), *pp);
        update_light_ssbo(current_frame_, scene);
        update_fog_volume_ssbo(current_frame_);
        render_scene_to(cmd, *vp.target, scene, dt, selected_id, pool, pp,
                        vp.skip_shadows, /*flip_tonemap_y*/ false, vp.viewport_graph,
                        VK_NULL_HANDLE, vp.backdrop);
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
    VkCommandBuffer swap_cmd = (post_cmd != VK_NULL_HANDLE && recorded_async_this_frame_)
        ? post_cmd : cmd;

    // --- Render ImGui to swapchain ---
    {
        // Transition swapchain image: UNDEFINED → COLOR_ATTACHMENT_OPTIMAL
        transition_image(swap_cmd, gpu_.swapchain().image(image_index),
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        VkRenderingAttachmentInfo color_attachment{};
        color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color_attachment.imageView = gpu_.swapchain().image_view(image_index);
        color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color_attachment.clearValue.color = {{0.012f, 0.012f, 0.015f, 1.0f}};

        VkRenderingInfo rendering_info{};
        rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering_info.renderArea.offset = {0, 0};
        rendering_info.renderArea.extent = gpu_.swapchain().extent();
        rendering_info.layerCount = 1;
        rendering_info.colorAttachmentCount = 1;
        rendering_info.pColorAttachments = &color_attachment;

        vkCmdBeginRendering(swap_cmd, &rendering_info);

        if (overlay_callback_) {
            overlay_callback_(swap_cmd);
        }

        vkCmdEndRendering(swap_cmd);

        // Transition swapchain image: COLOR_ATTACHMENT_OPTIMAL → PRESENT_SRC_KHR
        transition_image(swap_cmd, gpu_.swapchain().image(image_index),
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);
    }

#ifdef FJELL_ENABLE_TRACY
    TracyVkCollect(gpu_.tracy_ctx(), swap_cmd);
#endif

    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        FJELL_GFX_CRITICAL("Failed to end recording command buffer");
    }

    if (async_cmd != VK_NULL_HANDLE) {
        if (vkEndCommandBuffer(async_cmd) != VK_SUCCESS) {
            FJELL_GFX_CRITICAL("Failed to end recording async compute command buffer");
        }
    }
    if (post_cmd != VK_NULL_HANDLE) {
        if (vkEndCommandBuffer(post_cmd) != VK_SUCCESS) {
            FJELL_GFX_CRITICAL("Failed to end recording post-compute graphics command buffer");
        }
    }
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
                                          Scene* scene, VkExtent2D extent,
                                          PostProcessResources& pp) {
    FJELL_PROFILE_SCOPE_N("update_uniform_buffer");

    ViewHistory& history = pp.view_history();

    // TAA sub-pixel jitter: offset the projection matrix by ±0.5 pixels
    bool taa_enabled = pp.config().taa;
    glm::mat4 jittered_projection = projection;
    float jx = 0.0f;
    float jy = 0.0f;
    if (taa_enabled && extent.width > 0 && extent.height > 0) {
        int idx = static_cast<int>((history.taa_frame_count++) % 16) + 1; // 1-based, 16-sample cycle
        jx = (halton(idx, 2) - 0.5f) / static_cast<float>(extent.width);
        jy = (halton(idx, 3) - 0.5f) / static_cast<float>(extent.height);
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
    ubo.prev_unjittered_vp = history.unjittered_view_proj;
    ubo.jitter_x = jx;
    ubo.jitter_y = jy;
    // Point and spot lights are counted here; the directional light comes
    // resolved from the environment.
    uint32_t point_count = 0;
    uint32_t spot_count = 0;
    if (scene) {
        for (const auto& obj : scene->objects()) {
            if (dynamic_cast<const PointLightNode*>(obj.get())) {
                ++point_count;
            } else if (dynamic_cast<const SpotLightNode*>(obj.get())) {
                ++spot_count;
            }
        }
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
    ubo.ambient_color = glm::vec4(environment.ambient_color,
                                  environment.ambient_intensity * ibl_intensity_);

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
    // DDGI: sun bounce indirect
    if (pp.config().ddgi) {
        ubo.gi_intensity = ddgi_pass().config().gi_intensity;
    } else {
        ubo.gi_intensity = 0.0f;
    }
    // SS-GI: emissive color bleeding (supplements DDGI)
    if (pp.config().ss_gi && pp.ss_gi() && !pp.ss_gi()->first_frame) {
        ubo.ss_gi_intensity = ss_gi_pass().config().intensity;
    } else {
        ubo.ss_gi_intensity = 0.0f;
    }
    ubo.debug_light_channel = debug_light_channel_;

    // Forward+ tile info
    uint32_t tiles_x = (extent.width + LIGHT_TILE_SIZE - 1) / LIGHT_TILE_SIZE;
    uint32_t tiles_y = (extent.height + LIGHT_TILE_SIZE - 1) / LIGHT_TILE_SIZE;
    ubo.tile_info = glm::uvec4(tiles_x, tiles_y, point_count, spot_count);
    ubo.inv_projection = glm::inverse(jittered_projection);
    ubo.inv_view_proj = glm::inverse(jittered_projection * view);
    ubo.prev_view_proj = history.view_proj;
    ubo.prev_view = history.view;
    ubo.time = elapsed_time_;

    gpu_.uniform_buffer(frame_index).write(&ubo, sizeof(ubo));
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
    gpu_.environment_buffer(frame_index).write(&env_ubo, sizeof(env_ubo));

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

    auto* dst = static_cast<uint8_t*>(gpu_.light_ssbo(frame_index).mapped());
    uint32_t point_count = 0;
    uint32_t spot_count = 0;

    // Gather lights from scene nodes (position from world transform)
    for (const auto& obj : scene->objects()) {
        if (auto* pl = dynamic_cast<const PointLightNode*>(obj.get())) {
            if (point_count >= 1024) continue;
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
        } else if (auto* sl = dynamic_cast<const SpotLightNode*>(obj.get())) {
            if (spot_count >= 1024) continue;
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
        }
    }

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
    auto* dst = static_cast<GpuFogVolume*>(gpu_.fog_volume_ssbo(frame_index).mapped());

    const uint32_t count = std::min(static_cast<uint32_t>(volumes.size()), MAX_FOG_VOLUMES);
    for (uint32_t i = 0; i < count; ++i) {
        const auto& v = volumes[i];
        GpuFogVolume gpu{};
        // The shader takes a world position into the volume's own space, where
        // the shape is the unit box or sphere, so the inverse is what it needs.
        gpu.inv_transform = glm::inverse(v.transform);
        gpu.color_density = glm::vec4(v.color, v.density);
        gpu.shape_falloff = glm::vec4(static_cast<float>(v.shape), v.edge_falloff, 0.0f, 0.0f);
        gpu.scatter_params = glm::vec4(v.scattering_mult, 0.0f, 0.0f, 0.0f);

        // Already integrated by EnvironmentSubsystem::advance_fog_volumes, so
        // that drift stays continuous while the wind turns it.
        gpu.noise_drift = glm::vec4(v.noise_drift, v.noise_scale);
        gpu.noise_shape = glm::vec4(v.noise_contrast, static_cast<float>(v.noise_octaves),
                                    v.motion_response, 0.0f);
        dst[i] = gpu;
    }
}

void VulkanContext::init_game_ui(GameUi& ui, VkFormat output_format, bool world) {
    PassResources res{};
    res.global_set_layout = gpu_.global_set_layout();
    res.material_set_layout = gpu_.bindless_textures().layout();
    res.draw_data_layout = gatherer_.draw_data_descriptor().layout();
    res.meshlet_layout = gpu_.meshlet_descriptor().layout();
    res.depth_format = gpu_.device_ref().find_depth_format();
    res.color_format = OffscreenPass::COLOR_FORMAT;
    res.sample_count = gpu_.msaa_samples();

    ui.create(gpu_.device_ref(), gpu_.allocator(), res, gpu_.bindless_textures(),
              *font_library_, gpu_.pipeline_cache(), gpu_.deferred_deleter(), output_format);
    if (world) ui.create_world_pipelines(gpu_.device_ref(), res);
}

void VulkanContext::record_game_ui_offscreen(GameUi& ui, OffscreenPass& target,
                                              uint32_t frame_index) {
    if (!ui.loaded()) return;
    record_game_ui_offscreen_multi(
        [&ui](VkCommandBuffer cmd, VkDescriptorSet global_set, VkDescriptorSet bindless_set,
             VkExtent2D extent, uint32_t idx) {
            ui.record(cmd, global_set, bindless_set, extent, idx);
        },
        target, frame_index);
}

void VulkanContext::record_game_ui_offscreen_multi(const GameUiRecordFn& record_fn,
                                                    OffscreenPass& target,
                                                    uint32_t frame_index) {
    record_game_ui_into(gpu_.command_buffer(frame_index), record_fn, target, frame_index);
}

void VulkanContext::sync_scene_particles(Scene& scene) {
    if (particle_system_) particle_system_->sync_scene(scene, resource_manager_, project_);
}

void VulkanContext::release_scene_particles(const Scene& scene) {
    if (particle_system_) particle_system_->release_scene(scene.id());
}

void VulkanContext::render_game_ui_preview(GameUi& ui, OffscreenPass& target) {
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

    record_game_ui_into(cmd,
        [&ui](VkCommandBuffer c, VkDescriptorSet global_set, VkDescriptorSet bindless_set,
              VkExtent2D extent, uint32_t idx) {
            ui.record(c, global_set, bindless_set, extent, idx);
        },
        target, current_frame_);

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

void VulkanContext::record_game_ui_into(VkCommandBuffer cmd, const GameUiRecordFn& record_fn,
                                        OffscreenPass& target, uint32_t frame_index) {
    VkExtent2D extent = target.extent();

    // Transition offscreen color to color attachment
    VkImageMemoryBarrier to_attach{};
    to_attach.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_attach.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    to_attach.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    to_attach.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_attach.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_attach.image = target.color_image();
    to_attach.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0, 0, nullptr, 0, nullptr, 1, &to_attach);

    // Begin dynamic rendering targeting offscreen color
    VkRenderingAttachmentInfo color_att{};
    color_att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color_att.imageView = target.msaa_color_view();
    color_att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_att.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};

    // Resolve attachment if MSAA
    if (target.sample_count() != VK_SAMPLE_COUNT_1_BIT) {
        color_att.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        color_att.resolveImageView = target.color_view();
        color_att.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.extent = extent;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color_att;

    vkCmdBeginRendering(cmd, &ri);

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
    std::memcpy(static_cast<uint8_t*>(gpu_.uniform_buffer(frame_index).mapped()) +
                    offsetof(GlobalUbo, time),
                &live_time, sizeof(live_time));

    record_fn(cmd, gpu_.global_descriptor_set(frame_index),
              gpu_.bindless_textures().descriptor_set(),
              extent, frame_index);

    vkCmdEndRendering(cmd);

    // Transition to shader read for ImGui display
    VkImageMemoryBarrier to_read{};
    to_read.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_read.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    to_read.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_read.image = target.color_image();
    to_read.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &to_read);
}

void VulkanContext::submit_and_present() {
    if (!has_recorded_frame_) return;

    // Submit any uploads batched since the last flush. The frame submit
    // below waits on the buffer lane's timeline (the image lane shares the
    // graphics queue, where submission order already suffices).
    auto& upload = gpu_.upload_context();
    upload.flush();

    VkSemaphoreSubmitInfo upload_wait{};
    upload_wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    upload_wait.semaphore = upload.buffer_timeline();
    upload_wait.value = upload.last_buffer_value();
    upload_wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    auto& render_finished = gpu_.swapchain().render_finished_semaphores();
    VkSemaphore image_available = recorded_acquire_semaphore_;
    VkFence fence = gpu_.in_flight_fence(recorded_frame_);
    VkSemaphore timeline = gpu_.async_compute_timeline(recorded_frame_);

    // Fast path: no async work this frame → single graphics submit,
    // same as pre-Phase-3 behavior.
    if (!recorded_async_this_frame_) {
        std::array<VkSemaphoreSubmitInfo, 2> waits{};
        waits[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        waits[0].semaphore = image_available;
        waits[0].stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        waits[1] = upload_wait;

        VkCommandBufferSubmitInfo cmd_info{};
        cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmd_info.commandBuffer = gpu_.command_buffer(recorded_frame_);

        VkSemaphoreSubmitInfo signal{};
        signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signal.semaphore = render_finished[recorded_image_index_];
        signal.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

        VkSubmitInfo2 submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.waitSemaphoreInfoCount = static_cast<uint32_t>(waits.size());
        submit.pWaitSemaphoreInfos = waits.data();
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &cmd_info;
        submit.signalSemaphoreInfoCount = 1;
        submit.pSignalSemaphoreInfos = &signal;

        if (VkResult r = vkQueueSubmit2(gpu_.graphics_queue(), 1, &submit, fence); r != VK_SUCCESS) {
            FJELL_GFX_CRITICAL("vkQueueSubmit2 (single) failed: VkResult={}", static_cast<int>(r));
            if (r == VK_ERROR_DEVICE_LOST) gpu_.device_ref().dump_device_fault("graphics single submit");
            has_recorded_frame_ = false;
            return;
        }
    } else {
        // Async path: three submits chained by timeline semaphore.
        //   graphics_pre  : wait(image_available) → signal(timeline v1)
        //   compute       : wait(timeline v1)     → signal(timeline v2)
        //   graphics_post : wait(timeline v2)     → signal(render_finished) + fence
        //
        // v1 is the "depth is ready for compute" fence line; v2 is
        // "compute output is ready for graphics". Using two values per
        // frame keeps the sync unambiguous — compute doesn't risk
        // consuming the wrong graphics pre-output.
        //
        // Status (2026-04-27 Tracy session, showcase scene):
        // FJELL_DISABLE_ASYNC=1 measures ~5% *faster* (344 → 327 FPS) than
        // the async path. CPU work is identical; the regression is
        // GPU-side serialization plus 3× submit overhead. Suspect the
        // chain is effectively serial in practice — graphics_post waits
        // at FRAGMENT_SHADER for v2, so vertex/early fragment can run
        // alongside compute, but the workload here is too small for the
        // overlap to recover the per-submit and timeline cost.
        //
        // Three async passes total ~83µs of GPU compute; per-frame
        // delta is ~130µs. To make this pay we'd need either bigger
        // overlap candidates (DDGI probe update, bloom downsample) or
        // a deeper graphics tail to hide compute behind. Both deferred
        // until the workload supports them. FJELL_DISABLE_ASYNC=1 is
        // the A/B knob; the async-hint flags on GTAO/HiZ/LightCull stay
        // (the infrastructure is correct, just dormant on this scene).
        async_timeline_values_[recorded_frame_] += 2;
        uint64_t v1 = async_timeline_values_[recorded_frame_] - 1;
        uint64_t v2 = async_timeline_values_[recorded_frame_];

        // ── Submit 1: graphics pre ────────────────────────────────────
        std::array<VkSemaphoreSubmitInfo, 2> pre_waits{};
        pre_waits[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        pre_waits[0].semaphore = image_available;
        pre_waits[0].stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        pre_waits[1] = upload_wait;

        VkCommandBufferSubmitInfo pre_cmd{};
        pre_cmd.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        pre_cmd.commandBuffer = gpu_.command_buffer(recorded_frame_);

        VkSemaphoreSubmitInfo pre_signal{};
        pre_signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        pre_signal.semaphore = timeline;
        pre_signal.value = v1;
        // Signal from late-fragment stage — depth_prepass writes ended
        // by the end of LATE_FRAGMENT_TESTS, which is where compute
        // starts safely sampling depth from.
        pre_signal.stageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT
                              | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

        VkSubmitInfo2 pre_submit{};
        pre_submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        pre_submit.waitSemaphoreInfoCount = static_cast<uint32_t>(pre_waits.size());
        pre_submit.pWaitSemaphoreInfos = pre_waits.data();
        pre_submit.commandBufferInfoCount = 1;
        pre_submit.pCommandBufferInfos = &pre_cmd;
        pre_submit.signalSemaphoreInfoCount = 1;
        pre_submit.pSignalSemaphoreInfos = &pre_signal;

        if (VkResult r = vkQueueSubmit2(gpu_.graphics_queue(), 1, &pre_submit, VK_NULL_HANDLE); r != VK_SUCCESS) {
            FJELL_GFX_CRITICAL("vkQueueSubmit2 (graphics_pre) failed: VkResult={}", static_cast<int>(r));
            if (r == VK_ERROR_DEVICE_LOST) gpu_.device_ref().dump_device_fault("graphics_pre submit");
            has_recorded_frame_ = false;
            return;
        }

        // ── Submit 2: compute ────────────────────────────────────────
        VkSemaphoreSubmitInfo comp_wait{};
        comp_wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        comp_wait.semaphore = timeline;
        comp_wait.value = v1;
        comp_wait.stageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;

        VkCommandBufferSubmitInfo comp_cmd{};
        comp_cmd.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        comp_cmd.commandBuffer = gpu_.async_compute_cb(recorded_frame_);

        VkSemaphoreSubmitInfo comp_signal{};
        comp_signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        comp_signal.semaphore = timeline;
        comp_signal.value = v2;
        comp_signal.stageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;

        VkSubmitInfo2 comp_submit{};
        comp_submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        comp_submit.waitSemaphoreInfoCount = 1;
        comp_submit.pWaitSemaphoreInfos = &comp_wait;
        comp_submit.commandBufferInfoCount = 1;
        comp_submit.pCommandBufferInfos = &comp_cmd;
        comp_submit.signalSemaphoreInfoCount = 1;
        comp_submit.pSignalSemaphoreInfos = &comp_signal;

        if (VkResult r = vkQueueSubmit2(gpu_.async_compute_queue(), 1, &comp_submit, VK_NULL_HANDLE); r != VK_SUCCESS) {
            FJELL_GFX_CRITICAL("vkQueueSubmit2 (async compute) failed: VkResult={}", static_cast<int>(r));
            if (r == VK_ERROR_DEVICE_LOST) gpu_.device_ref().dump_device_fault("async compute submit");
            has_recorded_frame_ = false;
            return;
        }

        // ── Submit 3: graphics post ──────────────────────────────────
        VkSemaphoreSubmitInfo post_wait{};
        post_wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        post_wait.semaphore = timeline;
        post_wait.value = v2;
        // Wait at fragment stage — the compute output (AO) is read by
        // mesh.frag, so earlier graphics-queue work (if any were still
        // outstanding) wouldn't need to stall.
        post_wait.stageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;

        VkCommandBufferSubmitInfo post_cmd{};
        post_cmd.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        post_cmd.commandBuffer = gpu_.graphics_post_cb(recorded_frame_);

        VkSemaphoreSubmitInfo post_signal{};
        post_signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        post_signal.semaphore = render_finished[recorded_image_index_];
        post_signal.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

        VkSubmitInfo2 post_submit{};
        post_submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        post_submit.waitSemaphoreInfoCount = 1;
        post_submit.pWaitSemaphoreInfos = &post_wait;
        post_submit.commandBufferInfoCount = 1;
        post_submit.pCommandBufferInfos = &post_cmd;
        post_submit.signalSemaphoreInfoCount = 1;
        post_submit.pSignalSemaphoreInfos = &post_signal;

        if (VkResult r = vkQueueSubmit2(gpu_.graphics_queue(), 1, &post_submit, fence); r != VK_SUCCESS) {
            FJELL_GFX_CRITICAL("vkQueueSubmit2 (graphics_post) failed: VkResult={}", static_cast<int>(r));
            if (r == VK_ERROR_DEVICE_LOST) gpu_.device_ref().dump_device_fault("graphics_post submit");
            has_recorded_frame_ = false;
            return;
        }
    }

    VkPresentInfoKHR present_info{};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = &render_finished[recorded_image_index_];
    present_info.swapchainCount = 1;
    VkSwapchainKHR swapchain_handle = gpu_.swapchain().handle();
    present_info.pSwapchains = &swapchain_handle;
    present_info.pImageIndices = &recorded_image_index_;

    VkResult result = vkQueuePresentKHR(gpu_.device_ref().present_queue(), &present_info);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR ||
        gpu_.window().was_resized()) {
        gpu_.window().reset_resized();
        gpu_.swapchain().recreate();
    } else if (result != VK_SUCCESS) {
        FJELL_GFX_CRITICAL("vkQueuePresentKHR failed: {}", static_cast<int>(result));
    }

    target_pool_.end_frame();
    has_recorded_frame_ = false;
}

bool VulkanContext::begin_frame(ThreadPool& pool) {
    FJELL_PROFILE_SCOPE_N("begin_frame");
    auto vk_device = gpu_.device();

    // Advance to next frame slot
    current_frame_ = (current_frame_ + 1) % MAX_FRAMES_IN_FLIGHT;

    // Clean slate for async tracking. render_scene_to sets this when
    // any viewport actually records into the async CB.
    recorded_async_this_frame_ = false;

    VkFence frame_fence = gpu_.in_flight_fence(current_frame_);
    vkWaitForFences(vk_device, 1, &frame_fence, VK_TRUE,
                    std::numeric_limits<uint64_t>::max());

    // GPU is now done with this frame slot — safe to destroy/resize resources.
    apply_pending_ddgi_toggle();
    apply_pending_ddgi_probe_count();
    apply_ddgi_settle_rebuild();

    // Destroy resources retired the last time this slot recorded, and point
    // new retirements at this slot.
    gpu_.deferred_deleter().begin_frame(current_frame_);

    // A font whose recipe changed is rebuilt here, where the retired texture
    // lands in a slot the GPU is done with.
    font_library_->poll();

    // Free buffers that capacity growth retired for this slot last time it was
    // in flight. Safe now that the slot's fence has signaled — the command
    // buffers that referenced the old buffers have completed. Freeing them
    // inline at growth time (during record) would be a UAF.
    if (auto* batch = gatherer_.indirect_batch()) {
        batch->reclaim_retired(current_frame_);
    }
    for (uint32_t c = 0; c < FrameContext::MAX_SHADOW_CASCADES; ++c) {
        if (auto* sb = gatherer_.shadow_batch(c)) sb->reclaim_retired(current_frame_);
    }
    if (auto* mip = meshlet_instance_pass()) {
        mip->reclaim_retired(current_frame_);
    }
    // Free RT TLAS/instance buffers that build_tlas retired the last time this
    // slot was used — safe now the slot's fence has signaled. (Inline free in
    // build_tlas would UAF an RT trace recorded earlier in the same frame.)
    if (accel_manager_) {
        accel_manager_->reclaim_retired(current_frame_);

        // Declare how far each tracer's rays go, before anything builds the
        // TLAS from the union. Done here rather than inside the passes because
        // the one that builds the TLAS (DDGI, in the Color phase) records
        // before the one that traces furthest (reflections, in PostProcess):
        // a reach registered from the reflection pass's own record() would
        // arrive a frame late, every frame.
        accel_manager_->clear_trace_reach();
        if (rt_reflection_pass().ray_traced_active()) {
            accel_manager_->register_trace_reach(
                rt_reflection_pass().trace_reach_bounds(gatherer_.camera_position()));
        }
    }

    // Reclaim descriptor sets from the previous iteration of this slot.
    gpu_.descriptor_cache().begin_frame(current_frame_);

    // Lazy-init per-thread command pools (needs thread count from pool)
    gpu_.ensure_secondary_pools(pool.thread_count());
    if (pool.thread_count() > 0) {
        gpu_.secondary_pools().reset_frame(current_frame_);
    }

    // Rotating acquire semaphore (owned by the swapchain, not the frame slot).
    // Decoupling it from current_frame_ means a skipped/failed frame can't leave
    // this slot's semaphore signaled-but-unwaited for the next reuse.
    VkSemaphore acquire_sem = gpu_.swapchain().next_acquire_semaphore();

    uint32_t image_index;
    VkResult result = vkAcquireNextImageKHR(vk_device, gpu_.swapchain().handle(),
                          std::numeric_limits<uint64_t>::max(),
                          acquire_sem, VK_NULL_HANDLE,
                          &image_index);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_NOT_READY) {
        gpu_.swapchain().recreate();
        has_recorded_frame_ = false;
        return false;  // fence stays signaled — next wait will pass through
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        FJELL_GFX_CRITICAL("vkAcquireNextImageKHR failed: {}", static_cast<int>(result));
        has_recorded_frame_ = false;
        return false;
    }

    // Reset fence only after successful acquire — ensures the fence stays
    // signaled on the error/recreate path so the next vkWaitForFences doesn't hang
    vkResetFences(vk_device, 1, &frame_fence);
    pending_image_index_ = image_index;
    pending_acquire_semaphore_ = acquire_sem;
    return true;
}

void VulkanContext::record_frame(const std::vector<ViewportRenderRequest>& viewports,
                                 const EnvironmentState& environment,
                                 Scene* scene, float dt,
                                 uint32_t selected_id, ThreadPool& pool) {
    FJELL_PROFILE_SCOPE_N("record_frame");
    elapsed_time_ += dt;

    gpu_.render_stats().reset();
    // Always rebuild when there's a scene: update() is what clears the vertex
    // and command buffers, so skipping it on the frame a source turns off
    // leaves that frame's geometry on screen until something else redraws.
    if (scene) {
        debug_draw_->update(*scene, current_frame_, selected_id, show_colliders_, debug_draw_queue_);
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
            for (const auto& obj : s.objects()) {
                auto* spatial = obj->as_spatial();
                if (!spatial) continue;

                // Terrain has no Model, so it never reaches the mesh branch
                // below, but it is the largest thing in most scenes. Leaving it
                // out sizes the SDF volume and the DDGI probe grid as though the
                // ground were not there.
                if (auto* tc = obj->components.get<TerrainComponent>();
                    tc != nullptr && tc->terrain != nullptr) {
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
                        scene_sdf_pass().gather_instance(
                            SceneSdfPass::instance_key(s.id(), obj->id), terrain_bounds,
                            spatial->world_dirty_, inst);
                    }
                }

                auto* mesh_comp = obj->components.get<MeshComponent>();
                if (!mesh_comp || !mesh_comp->model) continue;

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
                    scene_sdf_pass().gather_instance(
                        SceneSdfPass::instance_key(s.id(), obj->id),
                        world_aabb, spatial->world_dirty_, inst);
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
                ddgi_settle_countdown_ = 30;
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
                    ddgi_settle_countdown_ = 30;
                    ddgi_grid_min_ = padded_min;
                    ddgi_grid_max_ = padded_min
                        + glm::vec3(ddgi_cfg.probe_counts - glm::ivec3(1)) * spacing;
                    FJELL_GFX_DEBUG("DDGI grid re-fitted to ({:.1f},{:.1f},{:.1f})..({:.1f},{:.1f},{:.1f})",
                                    padded_min.x, padded_min.y, padded_min.z,
                                    ddgi_grid_max_.x, ddgi_grid_max_.y, ddgi_grid_max_.z);
                }
            }
        }

        scene_sdf_pass().end_gather();
    }

    // Update per-frame global bindings before recording
    if (!viewports.empty() && viewports[0].target) {
        auto& gb = gpu_.global_bindings();

        gb.write_image(current_frame_, "depth_texture",
            viewports[0].target->depth_view(), gpu_.depth_sampler(),
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);

        VkImageView ao_view = default_pp_->gtao() ? default_pp_->gtao()->ao_view : gtao_pass().ao_view();
        gb.write_image(current_frame_, "ao_texture",
            ao_view, gtao_pass().ao_sampler(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        gb.write_image(current_frame_, "screen_color",
            viewports[0].target->screen_color_view(), tonemap_pass().output_sampler(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        // GI indirect: DDGI handles sun bounce indirect.
        //
        // The enabled check matters as much as the null check. A disabled
        // pass stops writing but its image keeps whatever it last produced,
        // and binding that composites a frozen frame of indirect light over
        // everything — a ghost that follows the camera because it is screen
        // space. Falling back to the black fallback image is what actually
        // turns the contribution off.
        VkImageView gi_view = gpu_.gi_fallback_view();
        auto* ddgi_imgs = default_pp_->ddgi();
        if (ddgi_pass().config().enabled
            && ddgi_imgs && ddgi_imgs->indirect_view != VK_NULL_HANDLE
            && !ddgi_imgs->images_need_init) {
            gi_view = ddgi_imgs->indirect_view;
        }
        gb.write_image(current_frame_, "gi_indirect",
            gi_view, gpu_.gi_fallback_sampler(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        // SS-GI emissive: screen-space emissive color bleeding (supplements DDGI)
        VkImageView ss_gi_view = gpu_.gi_fallback_view();
        auto* ss_gi_imgs = default_pp_->ss_gi();
        // Same ghost as DDGI above: a disabled pass keeps its last output.
        if (ss_gi_pass().config().enabled
            && ss_gi_imgs && ss_gi_imgs->indirect_view != VK_NULL_HANDLE
            && !ss_gi_imgs->first_frame) {
            ss_gi_view = ss_gi_imgs->indirect_view;
        }
        gb.write_image(current_frame_, "ss_gi_emissive",
            ss_gi_view, gpu_.gi_fallback_sampler(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        gb.write_image(current_frame_, "contact_shadow",
            contact_shadow_pass().shadow_view(),
            contact_shadow_pass().shadow_sampler(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        gb.flush_writes();
    }

    vkResetCommandBuffer(gpu_.command_buffer(current_frame_), 0);
    record_command_buffer(gpu_.command_buffer(current_frame_), pending_image_index_,
                          viewports, environment, scene, dt, selected_id, pool);

    recorded_frame_ = current_frame_;
    recorded_image_index_ = pending_image_index_;
    recorded_acquire_semaphore_ = pending_acquire_semaphore_;
    has_recorded_frame_ = true;
}

void VulkanContext::wait_idle() {
    vkDeviceWaitIdle(gpu_.device());
}

void VulkanContext::release_imgui_textures() {
    // Anything retired in the last frames — a font atlas swapped out, an
    // editor preview target — may hold an ImGui image and would otherwise
    // return it from the deleter after the backend is gone.
    vkDeviceWaitIdle(gpu_.device());
    gpu_.deferred_deleter().flush_all();
    font_library_->release_imgui_textures();
    default_pp_->invalidate_imgui_textures();
    depth_viz_pass().invalidate_imgui_texture();
    wireframe_pass().invalidate_imgui_texture();
    normal_pass().invalidate_imgui_texture();
    overdraw_pass().invalidate_imgui_texture();
    uv_viz_pass().invalidate_imgui_texture();
    if (auto* mv = passes_.try_get<MeshletVizPass>("meshlet_viz")) mv->invalidate_imgui_texture();
    if (auto* cv = passes_.try_get<CullVizPass>("cull_viz")) cv->invalidate_imgui_texture();
    light_heatmap_pass().invalidate_imgui_texture();
    light_grid_pass().invalidate_imgui_texture();
}

void VulkanContext::draw_sdf_debug_window() {
    if (!show_sdf_debug_) return;

    if (ImGui::Begin("SDF Debug", &show_sdf_debug_)) {
        const char* axes[] = {"X", "Y", "Z"};
        ImGui::Combo("Axis", &sdf_debug_axis_, axes, 3);
        ImGui::SliderFloat("Slice", &sdf_debug_slice_, 0.0f, 1.0f);

        glm::vec3 vol_min = scene_sdf_pass().volume_min();
        glm::vec3 vol_max = scene_sdf_pass().volume_max();
        bool bounds_changed = false;
        if (ImGui::DragFloat3("Vol Min", &vol_min.x, 0.5f)) bounds_changed = true;
        if (ImGui::DragFloat3("Vol Max", &vol_max.x, 0.5f)) bounds_changed = true;
        if (bounds_changed) scene_sdf_pass().set_bounds(vol_min, vol_max);

        ImGui::Text("Resolution: %u", scene_sdf_pass().resolution());

        // Dispatch slice render
        VkCommandBuffer cmd{};
        VkCommandBufferAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc.commandPool = gpu_.command_pool();
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        vkAllocateCommandBuffers(gpu_.device(), &alloc, &cmd);

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);

        scene_sdf_pass().render_slice(cmd, static_cast<uint32_t>(sdf_debug_axis_), sdf_debug_slice_);

        vkEndCommandBuffer(cmd);
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        vkQueueSubmit(gpu_.graphics_queue(), 1, &submit, VK_NULL_HANDLE);
        vkQueueWaitIdle(gpu_.graphics_queue());
        vkFreeCommandBuffers(gpu_.device(), gpu_.command_pool(), 1, &cmd);

        // Display slice image
        auto imgui_set = scene_sdf_pass().slice_imgui_set();
        if (imgui_set) {
            float avail = ImGui::GetContentRegionAvail().x;
            float size = std::min(avail, 256.0f);
            ImGui::Image(imgui_set, {size, size});
        }
    }
    ImGui::End();
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
    auto* tm = default_pp_->tonemap();
    if (tm && tm->extent.width > 0) {
        w = tm->extent.width;
        h = tm->extent.height;
    }

    // Resize triggers create/destroy of DDGI images based on updated config
    default_pp_->resize(w, h);
}

void VulkanContext::rebuild_ddgi_resources() {
    submit_and_present();
    vkDeviceWaitIdle(gpu_.device());

    // Destroy first: resize() early-outs when neither the probe count nor
    // the screen size changed, and this rebuild must happen even then.
    if (auto* imgs = default_pp_->ddgi()) {
        imgs->invalidate_imgui_texture();
        imgs->destroy(gpu_.device(), gpu_.allocator());
    }

    uint32_t w = 256, h = 256;
    auto* tm = default_pp_->tonemap();
    if (tm && tm->extent.width > 0) {
        w = tm->extent.width;
        h = tm->extent.height;
    }
    default_pp_->resize(w, h);

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

void VulkanContext::apply_ddgi_settle_rebuild() {
    if (!ddgi_settle_rebuild_armed_) return;
    if (default_pp_ == nullptr) return;


    // DDGI off: disarm instead of waiting. Enabling it later goes through
    // the toggle path, which creates the images fresh against the loaded
    // scene — the same effect this rebuild exists to produce.
    if (!default_pp_->config().ddgi || !ddgi_pass().config().enabled) {
        ddgi_settle_rebuild_armed_ = false;
        return;
    }

    // The gates below describe a scene settling, but each can stall on a
    // scene it does not model. Past the deadline, rebuild regardless: a late
    // rebuild costs one device wait, while never rebuilding leaves the
    // misrender this mechanism exists to clear.
    if (ddgi_settle_deadline_ == std::chrono::steady_clock::time_point{}) {
        ddgi_settle_deadline_ = std::chrono::steady_clock::now()
                              + std::chrono::milliseconds(DDGI_SETTLE_DEADLINE_MS);
    }
    const bool past_deadline = std::chrono::steady_clock::now() >= ddgi_settle_deadline_;

    // Wait until the grid has held still: every refit pushes the countdown
    // back up, so reaching zero means content has stopped arriving and the
    // scene is in the state a rebuild is known to fix.
    if (!past_deadline && ddgi_settle_countdown_ > 0) {
        --ddgi_settle_countdown_;
        return;
    }

    // Refit silence is necessary but not sufficient: it only says the grid
    // stopped moving. Probes trace the scene SDF and integrate the sky, and
    // both are built after the first frames — rebuilding before they exist
    // seeds every probe from a placeholder and reproduces the misrender this
    // rebuild is here to clear. Gate on those two inputs directly.
    if (!past_deadline && scene_sdf_pass().view() == VK_NULL_HANDLE) {
        return;
    }

    // cubemap_view() falls back to the procedural sky, so a non-null view is
    // not proof the final sky is resident. Only wait when the scene actually
    // asks for an HDRI — otherwise the procedural sky IS the final one.
    if (!past_deadline && sky_wants_hdri_ && !sky_cubemap_pass().has_hdri()) {
        return;
    }

    ddgi_settle_rebuild_armed_ = false;
    FJELL_GFX_DEBUG("DDGI: post-load rebuild ({})",
                    past_deadline ? "deadline reached" : "SDF + sky ready");
    rebuild_ddgi_resources();
}

void VulkanContext::reload_shaders(const std::vector<std::string>& changed_paths) {
    auto* cache = gpu_.pipeline_cache();
    if (cache == nullptr) return;
    if (cache->reload_changed(changed_paths) == 0) return;
    // Depth variants are built from the same entries, so they follow whatever
    // was rebuilt. The graphics pipeline and the particle system read raw
    // stages rather than FJSL and are untouched by a .fjsl edit.
    sync_custom_depth_pipelines();
}

void VulkanContext::reload_pipeline() {
    gpu_.pipeline()->recreate();
    gpu_.pipeline_cache()->recreate_all();
    particle_system_->recreate_pipelines();
    sync_custom_depth_pipelines();
}

void VulkanContext::sync_custom_depth_pipelines() {
    auto* pc = gpu_.pipeline_cache();
    if (!pc) return;

    for (uint32_t id = 0; id < pc->shader_count(); ++id) {
        if (!pc->has_custom_vertex(id)) continue;

        auto* msd = passes_.try_get<MeshShaderDepthPass>("mesh_shader_depth");
        const auto& depth_mesh = pc->depth_mesh_spv(id);
        if (msd && !depth_mesh.empty()) {
            msd->register_custom_depth(id, depth_mesh);
        }

        const auto& depth_vert = pc->depth_vert_spv(id);
        if (!depth_vert.empty()) {
            depth_prepass().register_custom_depth(id, depth_vert);
        }

        const auto& shadow_vert = pc->shadow_vert_spv(id);
        if (!shadow_vert.empty()) {
            shadow_pass().register_custom_shadow(id, shadow_vert);
        }
    }
}

// ── Scene Viewports ──────────────────────────────────────────────────────

uint32_t VulkanContext::create_scene_viewport(const SceneViewportConfig& config) {
    uint32_t bindless_idx = gpu_.bindless_textures().reserve_index();

    PostProcessResources::PassRefs refs{
        hiz_pass(), gtao_pass(), volumetric_fog_pass(),
        tonemap_pass(), ddgi_pass(), ss_gi_pass(), taa_resolve_pass(),
        rt_reflection_pass()};

    auto viewport = std::make_unique<SceneViewport>(
        gpu_.device_ref(), gpu_.device(), gpu_.allocator(),
        config, refs, bindless_idx);
    // Private global set — keeps the viewport's view-dependent image bindings
    // off the shared per-frame set bound by the main command buffer.
    viewport->set_global_set(gpu_.global_bindings().allocate_set());

    scene_viewports_[config.name] = std::move(viewport);
    return bindless_idx;
}

void VulkanContext::destroy_scene_viewport(const std::string& name) {
    auto it = scene_viewports_.find(name);
    if (it == scene_viewports_.end()) return;
    wait_idle();
    gpu_.global_bindings().free_set(it->second->global_set());
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
        req.global_set = viewport->global_set();
        render_preview(req, environment, scene, dt, selected_id, pool);
        viewport->sync_bindless(gpu_.bindless_textures());
    }
}

} // namespace fjell
