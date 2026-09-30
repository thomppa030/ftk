#pragma once

#include "gpu/frame.hpp"
#include "gpu/swapchain.hpp"
#include "renderer/fjgraph_asset.hpp"
#include "renderer/gpu_context.hpp"
#include "renderer/pass_registry.hpp"
#include "renderer/render_pipeline.hpp"
#include "renderer/scene_gatherer.hpp"
#include "renderer/debug_draw.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass/scene_sdf_pass.hpp"
#include "renderer/systems/render_subsystem.hpp"
#include "renderer/uniform_types.hpp"
#include "renderer/systems/particle_system.hpp"
#include "renderer/pass/bloom_pass.hpp"
#include "renderer/pass/hiz_pass.hpp"
#include "renderer/pass/meshlet_instance_pass.hpp"
#include "renderer/pass/debug_pass.hpp"
#include "renderer/pass/depth_prepass.hpp"
#include "renderer/pass/depth_viz_pass.hpp"
#include "renderer/pass/grid_pass.hpp"
#include "renderer/accel_structure.hpp"
#include "renderer/pass/ddgi_pass.hpp"
#include "renderer/pass/water_pass.hpp"
#include "renderer/pass/local_shadow_pass.hpp"
#include "renderer/pass/taa_resolve_pass.hpp"
#include "renderer/pass/gtao_pass.hpp"
#include "renderer/pass/ss_gi_pass.hpp"
#include "renderer/pass/rt_reflection_pass.hpp"
#include "renderer/pass/rt_scene_pass.hpp"
#include "renderer/pass/ssr_pass.hpp"
#include "renderer/pass/motion_field_pass.hpp"
#include "renderer/pass/volumetric_fog_pass.hpp"
#include "renderer/pass/contact_shadow_pass.hpp"
#include "renderer/pass/sun_shafts_pass.hpp"
#include "renderer/pass/light_cull_pass.hpp"
#include "renderer/pass/light_viz_pass.hpp"
#include "renderer/pass/mesh_shader_depth_pass.hpp"
#include "renderer/pass/meshlet_viz_pass.hpp"
#include "renderer/pass/normal_pass.hpp"
#include "renderer/pass/overdraw_pass.hpp"
#include "renderer/pass/shadow_pass.hpp"
#include "renderer/pass/sky_pass.hpp"
#include "renderer/pass/sky_cubemap_pass.hpp"
#include "renderer/pass/post_pass.hpp"
#include "renderer/pass/screen_color_copy_pass.hpp"
#include "renderer/pass/distortion_pass.hpp"
#include "renderer/pass/subsystem_compute_pass.hpp"
#include "renderer/pass/uv_viz_pass.hpp"
#include "renderer/pass/wireframe_pass.hpp"
#include "renderer/post_process_resources.hpp"
#include "renderer/systems/sprite_renderer.hpp"
#include "renderer/systems/tilemap_renderer.hpp"
#include "renderer/scene_viewport.hpp"
#include "renderer/viewport_source.hpp"
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fjell {

class Project;
class Scene;
struct EnvironmentState;
class Input;
class ResourceManager;
class ThreadPool;

class VulkanContext {
public:
  explicit VulkanContext(Window &window);
  ~VulkanContext();

  VulkanContext(const VulkanContext &) = delete;
  VulkanContext &operator=(const VulkanContext &) = delete;
  VulkanContext(VulkanContext &&) = delete;
  VulkanContext &operator=(VulkanContext &&) = delete;

  void sync_sprite_textures(Scene &scene, ResourceManager &resources, const Project *project = nullptr);
  void sync_tilemap_textures(Scene &scene, ResourceManager &resources, const Project *project = nullptr);

  /// Submit the previously recorded frame and present it.
  /// No-op on the first call (nothing recorded yet).
  void submit_and_present();

  /// Wait for the next frame slot and acquire swapchain image.
  /// After this returns, the GPU is done with this frame slot — safe to
  /// destroy/resize pass resources. Returns false if the swapchain was
  /// recreated and the frame should be skipped.
  bool begin_frame(ThreadPool &pool);

  /// Record commands for the current frame. Call after begin_frame() and
  /// after any resize/ImGui work.
  /// The time everything drawn this frame is drawn at: the game clock's
  /// elapsed time, given once a frame where that clock ticks.
  ///
  /// Given rather than kept, because the simulation reads the same clock.
  /// Buoyancy asks where a wave is at the game clock's time and the shader
  /// draws it at this one, so they have to be one number and not two that are
  /// advanced alike: a frame that is ticked and not drawn — every swapchain
  /// recreation while a window is resized — advances one and not the other,
  /// and a boat then sits out of phase with the wave under it for good.
  void set_time(float game_elapsed) { elapsed_time_ = game_elapsed; }

  void record_frame(const std::vector<ViewportRenderRequest> &viewports,
                    const EnvironmentState &environment, Scene *scene,
                    float dt, uint32_t selected_id, ThreadPool &pool);

  void wait_idle();
  void reload_pipeline();
  /// Rebuild only the FJSL pipelines naming one of `changed_paths`. What does
  /// not name a changed file keeps its pipelines and its generation, so a
  /// consumer watching one — the sky, a UI material — is not told to rebuild
  /// something that did not change.
  void reload_shaders(const std::vector<std::string>& changed_paths);
  void sync_custom_depth_pipelines();

  /// Re-parse `path` and apply its `enabled` flags to the live pipeline.
  /// Pass-shape changes (adds, removes, param edits, queue/phase
  /// overrides) are detected and logged but not applied — those still
  /// require a restart in v1. Called by the inotify watcher in debug
  /// builds.
  void reload_fjgraph(const std::string& path);

  /// Get a cached `FjGraph` by file path. Loads on first request, caches
  /// for subsequent ones. Returns null on parse failure (logged). Used
  /// by SceneViewport to resolve its viewport graph at construction.
  /// `path` is interpreted relative to `base_path::dir()`.
  [[nodiscard]] const FjGraph* get_or_load_graph(const std::string& path);

  // ── Forwarding accessors to GpuContext ────────────────────────────────
  [[nodiscard]] GpuContext& gpu() { return gpu_; }
  [[nodiscard]] VkDevice device() const { return gpu_.device(); }
  [[nodiscard]] VkPhysicalDevice physical_device() const { return gpu_.physical_device(); }
  [[nodiscard]] VmaAllocator allocator() const { return gpu_.allocator(); }
  [[nodiscard]] VkCommandPool command_pool() const { return gpu_.command_pool(); }
  [[nodiscard]] VkQueue graphics_queue() const { return gpu_.graphics_queue(); }
  [[nodiscard]] UploadContext& upload_context() { return gpu_.upload_context(); }
  [[nodiscard]] BindlessTextures &bindless_textures() { return gpu_.bindless_textures(); }
  [[nodiscard]] VkInstance instance() const { return gpu_.instance(); }
  /// The main window's images' format, what ImGui and the game UI draw into.
  [[nodiscard]] gpu::Format swapchain_format() const;
  [[nodiscard]] uint32_t graphics_family() const { return gpu_.graphics_family(); }
  [[nodiscard]] const std::string &gpu_name() const { return gpu_.gpu_name(); }
  [[nodiscard]] RenderStats &render_stats() { return gpu_.render_stats(); }
  [[nodiscard]] Device &device_ref() { return gpu_.device_ref(); }
  /// The GPU interface's device.
  [[nodiscard]] gpu::Device &gpu_device() { return gpu_.core().gpu_device(); }
  [[nodiscard]] gpu::Samples msaa_samples() const { return gpu_.msaa_samples(); }
  [[nodiscard]] bool ray_tracing_supported() const { return gpu_.ray_tracing_supported(); }
  [[nodiscard]] bool async_compute_supported() const { return gpu_.async_compute_supported(); }
  /// The project's choice; whether a frame actually uses the compute queue
  /// also needs the device to have one.
  [[nodiscard]] bool async_compute_enabled() const { return async_compute_enabled_; }

  // Apply a project's RenderSettings blob in one shot — called by
  // Engine::init_project after passes are up. Walks the struct and
  // calls existing setters; MSAA is not applied (would require
  // swapchain/offscreen recreation) and stays as whatever the engine
  // initialized with.
  void set_render_settings(const struct RenderSettings& s);
  [[nodiscard]] DdgiBackend ddgi_requested_backend() const {
      return ddgi_requested_backend_;
  }

  /// Debug: isolate one lighting term as the final pixel color instead of
  /// the full composite (see DebugLightChannel in uniform_types.hpp).
  /// Runtime-only diagnostic — not part of RenderSettings, not persisted.
  void set_debug_light_channel(int channel) { debug_light_channel_ = channel; }
  [[nodiscard]] int debug_light_channel() const { return debug_light_channel_; }
  [[nodiscard]] MegaBuffer *mega_buffer() { return gpu_.mega_buffer(); }
  [[nodiscard]] MeshletBuffer *meshlet_buffer() { return gpu_.meshlet_buffer(); }
  [[nodiscard]] PipelineCache *pipeline_cache() { return gpu_.pipeline_cache(); }

  /// The frame-in-flight index the next record_frame()/offscreen-preview
  /// callback will use. Callers that build per-frame data outside the
  /// main record_frame() path (e.g. UiEditorContext's preview, which
  /// calls GameUi::update() during ImGui drawing rather than from the
  /// ui_preview_callback_) must pass this instead of a fixed index, or
  /// their per-frame-in-flight buffers desync from what actually gets
  /// rendered.
  [[nodiscard]] uint32_t current_frame_index() const { return current_frame_; }

  // ── Pass accessors (passes owned by PassRegistry) ──────────────────
  [[nodiscard]] DepthPrePass &depth_prepass() { return passes_.get<DepthPrePass>("depth_prepass"); }
  [[nodiscard]] ShadowPass &shadow_pass() { return passes_.get<ShadowPass>("shadow"); }
  [[nodiscard]] DebugPass &debug_pass() { return passes_.get<DebugPass>("debug"); }
  [[nodiscard]] SkyPass &sky_pass() { return passes_.get<SkyPass>("sky"); }
  [[nodiscard]] SkyCubemapPass &sky_cubemap_pass() { return passes_.get<SkyCubemapPass>("sky_cubemap"); }
  [[nodiscard]] MeshletInstancePass *meshlet_instance_pass() { return passes_.try_get<MeshletInstancePass>("meshlet_cull"); }
  [[nodiscard]] LightCullPass &light_cull_pass() { return passes_.get<LightCullPass>("light_cull"); }
  [[nodiscard]] SceneSdfPass &scene_sdf_pass() { return passes_.get<SceneSdfPass>("scene_sdf"); }
  [[nodiscard]] DepthVizPass &depth_viz_pass() { return passes_.get<DepthVizPass>("depth_viz"); }
  [[nodiscard]] WireframePass &wireframe_pass() { return passes_.get<WireframePass>("wireframe"); }
  [[nodiscard]] NormalPass &normal_pass() { return passes_.get<NormalPass>("normals"); }
  [[nodiscard]] OverdrawPass &overdraw_pass() { return passes_.get<OverdrawPass>("overdraw"); }
  [[nodiscard]] UvVizPass &uv_viz_pass() { return passes_.get<UvVizPass>("uv_viz"); }
  [[nodiscard]] HizPass &hiz_pass() { return passes_.get<HizPass>("hiz"); }
  [[nodiscard]] BloomCore &bloom_pass() { return passes_.get<BloomCore>("bloom_ds_0"); }
  [[nodiscard]] AoPass &gtao_pass() { return passes_.get<AoPass>("ao_trace"); }
  [[nodiscard]] SsrCore &ssr_pass() { return passes_.get<SsrCore>("ssr_trace"); }
  [[nodiscard]] RtReflectionPass &rt_reflection_pass() { return passes_.get<RtReflectionPass>("rt_reflection"); }
  [[nodiscard]] FogCore &volumetric_fog_pass() { return passes_.get<FogCore>("fog_medium"); }
  [[nodiscard]] MotionFieldPass &motion_field_pass() { return passes_.get<MotionFieldPass>("motion_field"); }
  [[nodiscard]] ContactShadowPass &contact_shadow_pass() { return passes_.get<ContactShadowPass>("contact_shadow"); }
  [[nodiscard]] SunShaftsPass &sun_shafts_pass() { return passes_.get<SunShaftsPass>("sun_shafts"); }
  [[nodiscard]] DdgiPass &ddgi_pass() { return passes_.get<DdgiPass>("ddgi"); }
  [[nodiscard]] SsGiPass &ss_gi_pass() { return passes_.get<SsGiPass>("ss_gi"); }
  [[nodiscard]] WaterPass &water_pass() { return passes_.get<WaterPass>("water"); }
  [[nodiscard]] TaaResolvePass &taa_resolve_pass() { return passes_.get<TaaResolvePass>("taa_resolve"); }
  [[nodiscard]] LocalShadowPass &local_shadow_pass() { return passes_.get<LocalShadowPass>("local_shadows"); }
  void set_ddgi_enabled(bool enabled) { pending_ddgi_toggle_ = enabled; has_pending_ddgi_toggle_ = true; }
  void apply_pending_ddgi_toggle();
  /// Rebuild the DDGI atlases for a changed probe count.
  void apply_pending_ddgi_probe_count();
  /// Has every editor view make its resources anew, a fresh probe field
  /// among them, and forces a grid re-fit.
  void rebuild_ddgi_resources();
  /// Decides what the frame's rays can hit: the reach of every active
  /// tracer from every viewport, the scene's surfaces within it, and the
  /// BLASes and TLAS instances over them, before anything is recorded.
  void prepare_ray_tracing(const std::vector<ViewportRenderRequest>& viewports, Scene* scene);
  [[nodiscard]] PostPass &post_pass() { return passes_.get<PostPass>("post"); }
  [[nodiscard]] LightVizPass &light_heatmap_pass() { return passes_.get<LightVizPass>("light_heatmap"); }
  [[nodiscard]] LightVizPass &light_grid_pass() { return passes_.get<LightVizPass>("light_grid"); }
  [[nodiscard]] MeshletVizPass *meshlet_viz_pass() { return passes_.try_get<MeshletVizPass>("meshlet_viz"); }
  [[nodiscard]] GridPass &grid_pass() { return passes_.get<GridPass>("grid"); }
  [[nodiscard]] ScreenColorCopyPass &screen_color_copy_pass() { return passes_.get<ScreenColorCopyPass>("screen_color_copy"); }
  [[nodiscard]] DistortionPass &distortion_pass() { return passes_.get<DistortionPass>("distortion"); }
  [[nodiscard]] SubsystemComputePass &subsystem_compute_pass() { return passes_.get<SubsystemComputePass>("subsystem_compute"); }
  [[nodiscard]] DebugDraw &debug_draw() { return *debug_draw_; }

  using OverlayCallback = std::function<void(gpu::RenderEncoder&)>;
  void set_overlay_callback(OverlayCallback callback) {
    overlay_callback_ = std::move(callback);
  }

  void set_active_render_scene(Scene* s) { active_render_scene_ = s; }
  void set_show_colliders(bool show) { show_colliders_ = show; }
  [[nodiscard]] bool show_colliders() const { return show_colliders_; }

  // SDF debug
  void set_playing(bool playing) { playing_ = playing; }

  /// The scene's content was replaced wholesale (a scene load): fit the
  /// DDGI probe grid to it afresh and start every probe over, since what
  /// they gathered belongs to the scene that left.
  void restart_ddgi_probes() {
      ddgi_grid_initialized_ = false;
      ddgi_pass().invalidate_probe_lighting();
  }
  [[nodiscard]] bool is_playing() const { return playing_; }

  /// Initialize a GameUi with the renderer's resources and font library.
  /// Ownership stays with the caller. `world` also builds the worldspace
  /// pipelines (HDR format + scene MSAA) for a widget drawn in the scene.
  void init_game_ui(class GameUi& ui, gpu::Format output_format = gpu::Format::rgba8_srgb,
                    bool world = false);

  /// Every font atlas on the GPU, shared by all GameUi instances.
  [[nodiscard]] class FontLibrary& font_library() { return *font_library_; }

  /// Record a GameUi into an offscreen pass, in the frame's first list
  /// (editor previews). Called between begin_frame() and submit.
  void record_game_ui_offscreen(class GameUi& ui, OffscreenPass& target,
                                 uint32_t frame_index);

  /// Same transient render target and transitions as record_game_ui_offscreen,
  /// but the caller supplies the draw calls instead of a single GameUi — used
  /// by play mode, which composites every screen-space UiSubsystem instance
  /// into one shared HUD target (the scope's encoder, the global and bindless
  /// groups, the target's size and the frame slot — same shape as
  /// GameUi::record).
  using GameUiRecordFn = FrameContext::UiCallback;
  void record_game_ui_offscreen_multi(const GameUiRecordFn& record_fn,
                                       OffscreenPass& target, uint32_t frame_index);

  /// Render a GameUi into an offscreen pass in its own one-shot submit and
  /// wait for it, outside the frame's lists: for thumbnails, which read the
  /// result back at once. Called before anything records into the frame,
  /// like render_preview_and_wait. The target's colour image is left ready
  /// to sample.
  void render_game_ui_preview(class GameUi& ui, OffscreenPass& target);

  /// Register a scene's emitters with the particle system, for a scene
  /// rendered outside the frame (a thumbnail). record_frame does this for
  /// the frame's scene; other scenes' emitters are left alone, the
  /// particle system keeps them per scene.
  void sync_scene_particles(Scene& scene);

  /// Drop the particle state a scene owned. Call before a scene that had
  /// emitters is destroyed, or its emitters sit in the particle system for
  /// good.
  /// Drops every emitter the particle system holds for `scene`. Call before
  /// a Scene object is replaced wholesale (scene load, play restore): the
  /// loaded content carries a new scene id, so nothing else would ever
  /// reclaim the old emitters.
  void release_scene_particles(const Scene& scene);

  /// Moves a scene's effects on by `dt` seconds. Call once per tick for a
  /// scene that is drawn, as physics is stepped: the first view of it that
  /// runs the subsystem compute pass records the step and every other view
  /// draws what it left, so time advances once however many views there are.
  void advance_scene_particles(const Scene& scene, float dt);

  /// Callback for worldspace UI rendering (color pass, after transparent objects).
  using WorldUiCallback = FrameContext::UiCallback;
  void set_world_ui_callback(WorldUiCallback cb) { world_ui_callback_ = std::move(cb); }

  /// Callback for UI editor preview rendering (offscreen, called during record_frame).
  using UiPreviewCallback = std::function<void(uint32_t frame_index)>;
  void set_ui_preview_callback(UiPreviewCallback cb) { ui_preview_callback_ = std::move(cb); }

  /// Callback for play mode's HUD target (see PlayController::record_hud).
  /// Same shape and same call site as ui_preview_callback_ — inside the main
  /// command buffer's recording scope, so record_game_ui_offscreen_multi's
  /// barriers land on a buffer that is actually open.
  void set_play_hud_callback(UiPreviewCallback cb) { play_hud_callback_ = std::move(cb); }

  /// Set by ViewportManager each frame — top-left of the active viewport in
  /// window pixels.
  void set_viewport_offset(glm::vec2 offset) { viewport_offset_ = offset; }

  /// Bitmask of ViewportSource values currently displayed. Viz passes skip
  /// recording when their source bit is not set.
  void set_active_sources(uint32_t mask) { active_sources_ = mask; }
  [[nodiscard]] uint32_t active_sources() const { return active_sources_; }
  [[nodiscard]] RenderPipeline& render_pipeline() { return render_pipeline_; }
  /// Post-process resources for an editor view of `width` × `height`, made
  /// with the settings every editor view shares. A view makes its own anew
  /// once view_resources_generation() moves on from the one it was made at,
  /// as a settings change (DDGI on or off, the probe count) moves it.
  [[nodiscard]] std::unique_ptr<PostProcessResources> make_view_resources(uint32_t width, uint32_t height);
  [[nodiscard]] uint64_t view_resources_generation() const { return view_resources_generation_; }

  void set_animation_system(class AnimationSystem* sys) { animation_system_ = sys; }
  void set_resource_manager(ResourceManager* rm);
  void set_project(const Project* p) {
    project_ = p;
    sky_cubemap_pass().set_project(p);
    sky_pass().set_project(p);
    // The cache resolves every .fjsl it compiles, and a game's shaders are
    // named exactly like the engine's, so it cannot find them without this.
    if (auto* cache = pipeline_cache()) {
      cache->set_project(p);
    }
  }
  void set_debug_draw_queue(class DebugDrawQueue* q) { debug_draw_queue_ = q; }
  [[nodiscard]] class DebugDrawQueue* debug_draw_queue() const { return debug_draw_queue_; }

  // ── Scene Viewports (render-to-texture for materials) ─────────────────
  /// Create a named scene viewport. Returns the bindless index of its output.
  [[nodiscard]] uint32_t create_scene_viewport(const SceneViewportConfig& config);
  void destroy_scene_viewport(const std::string& name);
  [[nodiscard]] SceneViewport* scene_viewport(const std::string& name);
  [[nodiscard]] std::vector<std::string> scene_viewport_names() const;
  [[nodiscard]] uint32_t scene_viewport_bindless_index(const std::string& name) const;

  /// Render all scene viewports. Call before the main frame's record_frame().
  void render_scene_viewports(Scene* scene, float dt, uint32_t selected_id,
                              const EnvironmentState& environment, ThreadPool& pool);

  // The render target, clear and barriers every offscreen UI draw shares,
  // recorded into whatever list is open: the frame's for previews and the
  // HUD, a one-shot for readbacks.
  void record_game_ui_into(gpu::CommandList& cmd, const GameUiRecordFn& record_fn,
                           OffscreenPass& target, uint32_t frame_index);

  /// Renders `req`, a view with a target, post-process resources and global
  /// set of its own, into the frame being recorded, ahead of the frame's own
  /// views. Called between begin_frame() and record_frame().
  void render_preview(const ViewportRenderRequest& req,
                      const EnvironmentState& environment, Scene* scene,
                      float dt, uint32_t selected_id, ThreadPool& pool);
  /// render_preview() in a list of its own, submitted and waited for before
  /// it returns: for a caller reading the picture back at once. Called
  /// before anything records into the frame, so the GPU runs the frame's
  /// work in the order it was recorded.
  void render_preview_and_wait(const ViewportRenderRequest& req,
                               const EnvironmentState& environment, Scene* scene,
                               float dt, uint32_t selected_id, ThreadPool& pool);

  /// The swapchain image of the last frame presented, for the editor's
  /// screenshot capture to read the composited frame back from. A handle
  /// that finds nothing once the swapchain has been remade since.
  [[nodiscard]] gpu::Texture presented_swapchain_texture() const { return presented_texture_; }

private:
  /// Whether `req` carries everything a preview needs, reporting what it
  /// lacks.
  [[nodiscard]] bool preview_complete(const ViewportRenderRequest &req) const;
  void
  record_command_buffer(VkCommandBuffer cmd, const gpu::SwapchainImage &image,
                        const std::vector<ViewportRenderRequest> &viewports,
                        const EnvironmentState &environment, Scene *scene,
                        float dt, uint32_t selected_id, ThreadPool &pool);
  /// Renders `view` into its target: what it carries besides the camera
  /// (shadows, graph, private global set, backdrop, grid) decides how.
  /// The frame's main view alone records into its async lists.
  void render_scene_to(VkCommandBuffer cmd, const ViewportRenderRequest &view,
                       Scene *scene, float dt, uint32_t selected_id,
                       ThreadPool &pool, PostProcessResources *pp, bool main_view);
  /// Writes `view`'s uniforms, environment, lights and fog volumes into the
  /// frame's memory and points the global set the view binds at them and at
  /// its own depth, AO, GI, contact shadows and screen colour: the slot's set
  /// for the frame's views, the view's own for a preview. Each view's run
  /// binds a version of the set of its own, so none reads another's.
  void write_view_globals(const ViewportRenderRequest &view, const EnvironmentState &environment,
                          Scene *scene, PostProcessResources &pp);
  /// The GlobalUbo of a view, which also hands its camera to the gatherer.
  /// `pp` is the view's resources: its config decides which temporal effects
  /// the UBO enables (TAA jitter, GI intensities) and its ViewHistory
  /// supplies and receives the previous-frame camera.
  [[nodiscard]] GlobalUbo view_uniforms(const glm::mat4 &view, const glm::mat4 &projection,
                                        const glm::vec3 &camera_position,
                                        const EnvironmentState &environment, Scene *scene,
                                        glm::uvec2 extent, PostProcessResources &pp);
  /// The scene's point and spot lights in the frame's memory, laid out as
  /// the light SSBO.
  [[nodiscard]] gpu::BufferRange write_lights(Scene *scene);
  /// The environment's fog volumes in the frame's memory.
  [[nodiscard]] gpu::BufferRange write_fog_volumes(const EnvironmentState &environment);

  // `view.global_set` overrides the per-frame global descriptor set bound for
  // the frame (used by previews that own a private set); null = per-frame set.
  /// `view.viewport_graph` decides which passes this call runs, and so
  /// whether the GPU-driven meshlet buffers may be handed to the color
  /// passes; null = the global pass state governs.
  FrameContext build_frame_context(VkCommandBuffer cmd, const ViewportRenderRequest &view,
                                   Scene *scene, float dt, uint32_t selected_id,
                                   PostProcessResources *pp);

  // ── GPU infrastructure ────────────────────────────────────────────────
  GpuContext gpu_;
  RenderPipeline render_pipeline_;
  ResourceRegistry resource_registry_;
  FrameContext current_ctx_{};
  // The environment of the scene being recorded, handed to every FrameContext
  // built until the next record_frame or render_preview call replaces it.
  const EnvironmentState* current_environment_{nullptr};

  // ── Passes (owned by registry) ─────────────────────────────────────
  PassRegistry passes_;

  // The .fjgraph that built the current pipeline. Held so the hot-reload
  // watcher (debug builds only) can diff a re-parsed graph against it.
  FjGraph loaded_graph_;

  // Cache of additional graphs loaded for SceneViewports. Keyed by the
  // path passed to get_or_load_graph (resolved through base_path).
  // Lifetime is the VulkanContext's; viewports hold raw pointers into
  // this map and unordered_map guarantees pointer stability under insert.
  std::unordered_map<std::string, FjGraph> graph_cache_;

  // Particles / sprites
  // Rendering subsystems (particles, sprites, tilemaps, etc.)
  std::vector<std::unique_ptr<RenderSubsystem>> render_subsystems_;
  std::vector<RenderSubsystem*> subsystem_ptrs_; // cached raw pointers for FrameContext
  ParticleSystem* particle_system_{};
  SpriteRenderer* sprite_renderer_{};
  TilemapRenderer* tilemap_renderer_{};

  // ── Scene gathering ────────────────────────────────────────────────────
  SceneGatherer gatherer_;

  // ── Frame state ───────────────────────────────────────────────────────
  /// What every editor view's post-process resources are made with, and
  /// how many times a settings change has asked them to be made anew.
  PostProcessConfig view_config_{};
  uint64_t view_resources_generation_{0};
  /// The frame begun and not yet recorded, and its slot.
  gpu::Frame* frame_{nullptr};
  uint32_t current_frame_{0};

  // Split-frame state: carried between begin_frame / record_frame / submit_and_present
  /// The frame recorded and not yet submitted, and the lists it recorded
  /// into: pre, and the compute and post lists where there is a compute
  /// queue.
  gpu::Frame* recorded_gpu_frame_{nullptr};
  gpu::CommandList* pre_list_{nullptr};
  gpu::CommandList* async_list_{nullptr};
  gpu::CommandList* post_list_{nullptr};
  /// Those lists' command buffers, which the frame's recording writes.
  VkCommandBuffer frame_cmd_{VK_NULL_HANDLE};
  VkCommandBuffer async_cmd_{VK_NULL_HANDLE};
  VkCommandBuffer post_cmd_{VK_NULL_HANDLE};
  /// The swapchain image `frame_` draws into, and the one
  /// `recorded_gpu_frame_` drew into, which it presents.
  gpu::SwapchainImage acquired_image_{};
  gpu::SwapchainImage recorded_image_{};
  gpu::Texture presented_texture_{};
  bool has_recorded_frame_{false};
  // True when the main-viewport pipeline actually recorded commands
  // into the async compute CB this frame — drives whether submit_and_
  // present fires the compute queue submit + timeline wait.
  bool recorded_async_this_frame_{false};
  bool async_compute_enabled_{false};
  /// The images the view being recorded binds at the GI slots, null for the
  /// fallback; written with its bindings and handed to its graph.
  gpu::Texture bound_gi_indirect_{};
  gpu::Texture bound_ss_gi_emissive_{};
  /// The view being recorded's inverse view-projection, jitter included.
  glm::mat4 view_inv_view_proj_{1.0f};
  /// The serial of the frame begun last: what the transient pool dates its
  /// textures by.
  uint64_t frame_serial_{0};


  // ── Editor / game state ───────────────────────────────────────────────
  std::unique_ptr<DebugDraw> debug_draw_;
  std::unique_ptr<class FontLibrary> font_library_;
  DebugDrawQueue* debug_draw_queue_{nullptr};
  OverlayCallback overlay_callback_;
  WorldUiCallback world_ui_callback_;
  UiPreviewCallback ui_preview_callback_;
  UiPreviewCallback play_hud_callback_;
  glm::vec2 viewport_offset_{0.0f, 0.0f};
  bool show_colliders_{false};
  Scene* active_render_scene_{nullptr}; // editor context's preview scene (if different from main)
  bool playing_{false};
  bool has_pending_ddgi_toggle_{false};
  bool pending_ddgi_toggle_{false};
  /// A probe-count change waiting for a safe point to rebuild the atlases.
  /// Deferred for the same reason as the enable toggle: the images are in
  /// use by frames still in flight when the setting changes.
  bool has_pending_ddgi_probe_count_{false};
  bool ddgi_grid_initialized_{false};
  /// The DDGI backend the project asked for, before any downgrade for a
  /// device that cannot ray trace. Saving the downgraded value instead
  /// would discard the user's choice the moment the project is opened on
  /// such a device.
  DdgiBackend ddgi_requested_backend_{DdgiBackend::ray_traced};
  // World bounds the DDGI grid currently covers. Compared against scene
  // bounds each frame to decide whether content has escaped it.
  glm::vec3 ddgi_grid_min_{0.0f};
  glm::vec3 ddgi_grid_max_{0.0f};
  uint32_t active_sources_{~0u}; // all enabled by default
  AnimationSystem* animation_system_{nullptr};
  ResourceManager* resource_manager_{nullptr};
  std::unique_ptr<AccelStructureManager> accel_manager_;
  const Project* project_{nullptr};
  /// The game clock's elapsed time, as the engine last gave it. Shaders read
  /// it as `TIME`. Not advanced here: see set_time().
  float elapsed_time_{0.0f};
  int debug_light_channel_{0}; // see DebugLightChannel in uniform_types.hpp

  // ── Scene viewports (render-to-texture for materials) ─────────────────
  std::unordered_map<std::string, std::unique_ptr<SceneViewport>> scene_viewports_;
};

} // namespace fjell
