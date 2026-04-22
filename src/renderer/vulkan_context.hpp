#pragma once

#include "renderer/gpu_context.hpp"
#include "renderer/pass_registry.hpp"
#include "renderer/render_pipeline.hpp"
#include "renderer/scene_gatherer.hpp"
#include "renderer/debug_draw.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass/scene_sdf_pass.hpp"
#include "renderer/systems/render_subsystem.hpp"
#include "renderer/systems/particle_system.hpp"
#include "renderer/pass/bloom_pass.hpp"
#include "renderer/pass/cull_pass.hpp"
#include "renderer/pass/hiz_pass.hpp"
#include "renderer/pass/meshlet_instance_pass.hpp"
#include "renderer/pass/cull_viz_pass.hpp"
#include "renderer/pass/debug_pass.hpp"
#include "renderer/pass/depth_prepass.hpp"
#include "renderer/pass/depth_viz_pass.hpp"
#include "renderer/pass/grid_pass.hpp"
#include "renderer/gpu/accel_structure.hpp"
#include "renderer/pass/ddgi_pass.hpp"
#include "renderer/pass/local_shadow_pass.hpp"
#include "renderer/pass/taa_resolve_pass.hpp"
#include "renderer/pass/gtao_pass.hpp"
#include "renderer/pass/ss_gi_pass.hpp"
#include "renderer/pass/ssr_pass.hpp"
#include "renderer/pass/volumetric_fog_pass.hpp"
#include "renderer/pass/contact_shadow_pass.hpp"
#include "renderer/pass/light_cull_pass.hpp"
#include "renderer/pass/light_viz_pass.hpp"
#include "renderer/pass/mesh_shader_depth_pass.hpp"
#include "renderer/pass/meshlet_viz_pass.hpp"
#include "renderer/pass/normal_pass.hpp"
#include "renderer/pass/overdraw_pass.hpp"
#include "renderer/pass/shadow_pass.hpp"
#include "renderer/pass/sky_pass.hpp"
#include "renderer/pass/sky_cubemap_pass.hpp"
#include "renderer/pass/tonemap_pass.hpp"
#include "renderer/pass/transparent_pass.hpp"
#include "renderer/pass/opaque_pass.hpp"
#include "renderer/pass/screen_color_copy_pass.hpp"
#include "renderer/pass/distortion_pass.hpp"
#include "renderer/pass/subsystem_compute_pass.hpp"
#include "renderer/pass/uv_viz_pass.hpp"
#include "renderer/pass/wireframe_pass.hpp"
#include "renderer/post_process_resources.hpp"
#include "renderer/render_target_pool.hpp"
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
  void record_frame(const std::vector<ViewportRenderRequest> &viewports,
                    const struct DirectionalLight &light, Scene *scene,
                    float dt, uint32_t selected_id, ThreadPool &pool,
                    bool playing = false);

  void update_light_ssbo(uint32_t frame_index, Scene *scene);
  void wait_idle();
  void release_imgui_textures();
  void reload_pipeline();
  void sync_custom_depth_pipelines();

  // ── Forwarding accessors to GpuContext ────────────────────────────────
  [[nodiscard]] GpuContext& gpu() { return gpu_; }
  [[nodiscard]] VkDevice device() const { return gpu_.device(); }
  [[nodiscard]] VkPhysicalDevice physical_device() const { return gpu_.physical_device(); }
  [[nodiscard]] VmaAllocator allocator() const { return gpu_.allocator(); }
  [[nodiscard]] VkCommandPool command_pool() const { return gpu_.command_pool(); }
  [[nodiscard]] VkQueue graphics_queue() const { return gpu_.graphics_queue(); }
  [[nodiscard]] BindlessTextures &bindless_textures() { return gpu_.bindless_textures(); }
  [[nodiscard]] VkInstance instance() const { return gpu_.instance(); }
  [[nodiscard]] VkFormat swapchain_format() const { return gpu_.swapchain_format(); }
  [[nodiscard]] VkExtent2D swapchain_extent() const { return gpu_.swapchain_extent(); }
  [[nodiscard]] uint32_t image_count() const { return gpu_.image_count(); }
  [[nodiscard]] uint32_t graphics_family() const { return gpu_.graphics_family(); }
  [[nodiscard]] const std::string &gpu_name() const { return gpu_.gpu_name(); }
  [[nodiscard]] RenderStats &render_stats() { return gpu_.render_stats(); }
  [[nodiscard]] Device &device_ref() { return gpu_.device_ref(); }
  [[nodiscard]] VkSampleCountFlagBits msaa_samples() const { return gpu_.msaa_samples(); }
  [[nodiscard]] bool mesh_shaders_available() const { return gpu_.mesh_shaders_available(); }
  [[nodiscard]] bool use_mesh_shaders() const { return gpu_.use_mesh_shaders(); }
  void set_use_mesh_shaders(bool enable);

  // Apply a project's RenderSettings blob in one shot — called by
  // Engine::init_project after passes are up. Walks the struct and
  // calls existing setters; MSAA is not applied (would require
  // swapchain/offscreen recreation) and stays as whatever the engine
  // initialized with.
  void set_render_settings(const struct RenderSettings& s);
  [[nodiscard]] MegaBuffer *mega_buffer() { return gpu_.mega_buffer(); }
  [[nodiscard]] MeshletBuffer *meshlet_buffer() { return gpu_.meshlet_buffer(); }
  [[nodiscard]] PipelineCache *pipeline_cache() { return gpu_.pipeline_cache(); }

  // ── Pass accessors (passes owned by PassRegistry) ──────────────────
  [[nodiscard]] DepthPrePass &depth_prepass() { return passes_.get<DepthPrePass>("depth_prepass"); }
  [[nodiscard]] ShadowPass &shadow_pass() { return passes_.get<ShadowPass>("shadow"); }
  [[nodiscard]] DebugPass &debug_pass() { return passes_.get<DebugPass>("debug"); }
  [[nodiscard]] SkyPass &sky_pass() { return passes_.get<SkyPass>("sky"); }
  [[nodiscard]] SkyCubemapPass &sky_cubemap_pass() { return passes_.get<SkyCubemapPass>("sky_cubemap"); }
  [[nodiscard]] TransparentPass &transparent_pass() { return passes_.get<TransparentPass>("transparent"); }
  [[nodiscard]] MeshShaderDepthPass *mesh_shader_depth_pass() { return passes_.try_get<MeshShaderDepthPass>("mesh_shader_depth"); }
  [[nodiscard]] MeshletInstancePass *meshlet_instance_pass() { return passes_.try_get<MeshletInstancePass>("meshlet_instance"); }
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
  [[nodiscard]] FogCore &volumetric_fog_pass() { return passes_.get<FogCore>("fog_inject"); }
  [[nodiscard]] ContactShadowPass &contact_shadow_pass() { return passes_.get<ContactShadowPass>("contact_shadow"); }
  [[nodiscard]] DdgiPass &ddgi_pass() { return passes_.get<DdgiPass>("ddgi"); }
  [[nodiscard]] SsGiPass &ss_gi_pass() { return passes_.get<SsGiPass>("ss_gi"); }
  [[nodiscard]] TaaResolvePass &taa_resolve_pass() { return passes_.get<TaaResolvePass>("taa_resolve"); }
  [[nodiscard]] LocalShadowPass &local_shadow_pass() { return passes_.get<LocalShadowPass>("local_shadows"); }
  void set_ddgi_enabled(bool enabled) { pending_ddgi_toggle_ = enabled; has_pending_ddgi_toggle_ = true; }
  void apply_pending_ddgi_toggle();
  [[nodiscard]] TonemapPass &tonemap_pass() { return passes_.get<TonemapPass>("tonemap"); }
  [[nodiscard]] LightVizPass &light_heatmap_pass() { return passes_.get<LightVizPass>("light_heatmap"); }
  [[nodiscard]] LightVizPass &light_grid_pass() { return passes_.get<LightVizPass>("light_grid"); }
  [[nodiscard]] MeshletVizPass *meshlet_viz_pass() { return passes_.try_get<MeshletVizPass>("meshlet_viz"); }
  [[nodiscard]] CullVizPass *cull_viz_pass() { return passes_.try_get<CullVizPass>("cull_viz"); }
  [[nodiscard]] CullPass *cull_pass() { return passes_.try_get<CullPass>("cull"); }
  [[nodiscard]] GridPass &grid_pass() { return passes_.get<GridPass>("grid"); }
  [[nodiscard]] OpaquePass &opaque_pass() { return passes_.get<OpaquePass>("opaque"); }
  [[nodiscard]] ScreenColorCopyPass &screen_color_copy_pass() { return passes_.get<ScreenColorCopyPass>("screen_color_copy"); }
  [[nodiscard]] DistortionPass &distortion_pass() { return passes_.get<DistortionPass>("distortion"); }
  [[nodiscard]] SubsystemComputePass &subsystem_compute_pass() { return passes_.get<SubsystemComputePass>("subsystem_compute"); }
  [[nodiscard]] DebugDraw &debug_draw() { return *debug_draw_; }

  using OverlayCallback = std::function<void(VkCommandBuffer)>;
  void set_overlay_callback(OverlayCallback callback) {
    overlay_callback_ = std::move(callback);
  }

  void set_active_render_scene(Scene* s) { active_render_scene_ = s; }
  void set_show_colliders(bool show) { show_colliders_ = show; }
  [[nodiscard]] bool show_colliders() const { return show_colliders_; }

  // SDF debug
  void set_show_sdf_debug(bool show) { show_sdf_debug_ = show; }
  [[nodiscard]] bool show_sdf_debug() const { return show_sdf_debug_; }
  void draw_sdf_debug_window();
  void set_playing(bool playing) { playing_ = playing; }
  [[nodiscard]] bool is_playing() const { return playing_; }

  /// Initialize a GameUi instance using renderer resources.
  /// Ownership stays with the caller.
  void init_game_ui(class GameUi& ui, const std::string& font_path,
                    VkFormat output_format = VK_FORMAT_R8G8B8A8_SRGB);

  /// Initialize a GameUi using a shared font atlas (no msdfgen rebuild).
  void init_game_ui_shared(class GameUi& ui, const class FontAtlas& shared_atlas,
                           VkFormat output_format = VK_FORMAT_R8G8B8A8_SRGB);

  /// Callback for game UI rendering. Set by the game layer.
  /// Receives: cmd, global_set, bindless_set, extent, frame_index.
  using GameUiCallback = std::function<void(VkCommandBuffer, VkDescriptorSet,
                                            VkDescriptorSet, VkExtent2D, uint32_t)>;
  void set_game_ui_callback(GameUiCallback cb) { game_ui_callback_ = std::move(cb); }

  /// Record a GameUi into an offscreen pass (for editor preview).
  void record_game_ui_offscreen(class GameUi& ui, OffscreenPass& target,
                                 uint32_t frame_index);

  /// Callback for worldspace UI rendering (color pass, after transparent objects).
  using WorldUiCallback = std::function<void(VkCommandBuffer, VkDescriptorSet,
                                              VkDescriptorSet, VkExtent2D, uint32_t)>;
  void set_world_ui_callback(WorldUiCallback cb) { world_ui_callback_ = std::move(cb); }

  /// Initialize a GameUi for worldspace rendering (HDR format + scene MSAA).
  void init_game_ui_world(class GameUi& ui, const class FontAtlas& shared_atlas);

  /// Callback for UI editor preview rendering (offscreen, called during record_frame).
  using UiPreviewCallback = std::function<void(uint32_t frame_index)>;
  void set_ui_preview_callback(UiPreviewCallback cb) { ui_preview_callback_ = std::move(cb); }

  /// Set by ViewportManager each frame — top-left of the active viewport in
  /// window pixels.
  void set_viewport_offset(glm::vec2 offset) { viewport_offset_ = offset; }

  /// Bitmask of ViewportSource values currently displayed. Viz passes skip
  /// recording when their source bit is not set.
  void set_active_sources(uint32_t mask) { active_sources_ = mask; }
  [[nodiscard]] uint32_t active_sources() const { return active_sources_; }
  [[nodiscard]] RenderTargetPool& target_pool() { return target_pool_; }
  [[nodiscard]] RenderPipeline& render_pipeline() { return render_pipeline_; }
  [[nodiscard]] PostProcessResources& default_pp() { return *default_pp_; }

  void set_animation_system(class AnimationSystem* sys) { animation_system_ = sys; }
  void set_resource_manager(ResourceManager* rm) { resource_manager_ = rm; }
  void set_project(const Project* p) { project_ = p; sky_cubemap_pass().set_project(p); }
  void set_debug_draw_queue(class DebugDrawQueue* q) { debug_draw_queue_ = q; }
  [[nodiscard]] class DebugDrawQueue* debug_draw_queue() const { return debug_draw_queue_; }

  // ── Scene Viewports (render-to-texture for materials) ─────────────────
  /// Create a named scene viewport. Returns the bindless index of its output.
  [[nodiscard]] uint32_t create_scene_viewport(const SceneViewportConfig& config);
  void destroy_scene_viewport(const std::string& name);
  [[nodiscard]] SceneViewport* scene_viewport(const std::string& name);
  [[nodiscard]] uint32_t scene_viewport_bindless_index(const std::string& name) const;

  /// Render all scene viewports. Call before the main frame's record_frame().
  void render_scene_viewports(Scene* scene, float dt, uint32_t selected_id,
                              const DirectionalLight& light, ThreadPool& pool);

  /// Render a single viewport in its own command buffer (submitted + waited).
  /// Used for camera preview to avoid singleton state conflicts with the main
  /// viewport's render_scene_to in the same command buffer.
  void render_preview(const ViewportRenderRequest& req,
                      const DirectionalLight& light, Scene* scene,
                      float dt, uint32_t selected_id, ThreadPool& pool);

private:
  void
  record_command_buffer(VkCommandBuffer cmd, uint32_t image_index,
                        const std::vector<ViewportRenderRequest> &viewports,
                        const struct DirectionalLight &light, Scene *scene,
                        float dt, uint32_t selected_id, ThreadPool &pool,
                        bool playing);
  void render_scene_to(VkCommandBuffer cmd, OffscreenPass &target, Scene *scene,
                       float dt, uint32_t selected_id, ThreadPool &pool,
                       PostProcessResources *pp, bool skip_shadows = false,
                       bool flip_tonemap_y = false);
  void update_uniform_buffer(uint32_t frame_index, const glm::mat4 &view,
                             const glm::mat4 &projection,
                             const glm::vec3 &camera_position,
                             const struct DirectionalLight &light, Scene *scene,
                             VkExtent2D extent);

  void register_passes();

  FrameContext build_frame_context(VkCommandBuffer cmd, OffscreenPass &target,
                                   Scene *scene, float dt, uint32_t selected_id,
                                   PostProcessResources *pp, bool skip_shadows,
                                   bool flip_tonemap_y);

  // ── GPU infrastructure ────────────────────────────────────────────────
  GpuContext gpu_;
  RenderPipeline render_pipeline_;
  ResourceRegistry resource_registry_;
  RenderTargetPool target_pool_;
  FrameContext current_ctx_{};

  // ── Passes (owned by registry) ─────────────────────────────────────
  PassRegistry passes_;

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
  std::unique_ptr<PostProcessResources> default_pp_;
  uint32_t current_frame_{MAX_FRAMES_IN_FLIGHT - 1};

  // Split-frame state: carried between begin_frame / record_frame / submit_and_present
  uint32_t recorded_frame_{0};
  uint32_t recorded_image_index_{0};
  uint32_t pending_image_index_{0};
  bool has_recorded_frame_{false};
  // True when the main-viewport pipeline actually recorded commands
  // into the async compute CB this frame — drives whether submit_and_
  // present fires the compute queue submit + timeline wait.
  bool recorded_async_this_frame_{false};
  // Per-frame-slot monotonic counter for timeline semaphore values.
  // Compute submit signals `next`, graphics waits on `next`, then we
  // advance. Separate per slot so MAX_FRAMES_IN_FLIGHT concurrent frames
  // don't stomp on each other.
  std::array<uint64_t, MAX_FRAMES_IN_FLIGHT> async_timeline_values_{};


  // ── Editor / game state ───────────────────────────────────────────────
  std::unique_ptr<DebugDraw> debug_draw_;
  DebugDrawQueue* debug_draw_queue_{nullptr};
  OverlayCallback overlay_callback_;
  GameUiCallback game_ui_callback_;
  WorldUiCallback world_ui_callback_;
  UiPreviewCallback ui_preview_callback_;
  glm::vec2 viewport_offset_{0.0f, 0.0f};
  bool show_colliders_{false};
  Scene* active_render_scene_{nullptr}; // editor context's preview scene (if different from main)
  std::vector<GpuSdfInstance> pending_sdf_instances_;
  bool show_sdf_debug_{false};
  int sdf_debug_axis_{1};       // default Y axis
  float sdf_debug_slice_{0.5f};
  bool playing_{false};
  bool meshlet_passes_created_{false};
  bool has_pending_ddgi_toggle_{false};
  bool pending_ddgi_toggle_{false};
  bool ddgi_grid_initialized_{false};
  uint32_t active_sources_{~0u}; // all enabled by default
  AnimationSystem* animation_system_{nullptr};
  ResourceManager* resource_manager_{nullptr};
  std::unique_ptr<AccelStructureManager> accel_manager_;
  const Project* project_{nullptr};
  float ibl_intensity_{1.0f};
  float elapsed_time_{0.0f};

  // ── TAA jitter state ─────────────────────────────────────────────────
  uint32_t taa_frame_count_{0};
  glm::mat4 prev_unjittered_vp_{1.0f};

  // ── Scene viewports (render-to-texture for materials) ─────────────────
  std::unordered_map<std::string, std::unique_ptr<SceneViewport>> scene_viewports_;
};

} // namespace fjell
