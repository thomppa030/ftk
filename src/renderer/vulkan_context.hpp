#pragma once

#include "renderer/gpu_context.hpp"
#include "renderer/render_pipeline.hpp"
#include "renderer/scene_gatherer.hpp"
#include "renderer/debug_draw.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass/scene_sdf_pass.hpp"
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
#include "renderer/pass/gtao_pass.hpp"
#include "renderer/pass/ssr_pass.hpp"
#include "renderer/pass/volumetric_fog_pass.hpp"
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
  [[nodiscard]] MegaBuffer *mega_buffer() { return gpu_.mega_buffer(); }
  [[nodiscard]] MeshletBuffer *meshlet_buffer() { return gpu_.meshlet_buffer(); }
  [[nodiscard]] PipelineCache *pipeline_cache() { return gpu_.pipeline_cache(); }

  // ── Pass accessors (still owned by VulkanContext) ─────────────────────
  [[nodiscard]] DepthVizPass &depth_viz_pass() { return depth_viz_pass_; }
  [[nodiscard]] WireframePass &wireframe_pass() { return wireframe_pass_; }
  [[nodiscard]] NormalPass &normal_pass() { return normal_pass_; }
  [[nodiscard]] OverdrawPass &overdraw_pass() { return overdraw_pass_; }
  [[nodiscard]] UvVizPass &uv_viz_pass() { return uv_viz_pass_; }
  [[nodiscard]] HizPass &hiz_pass() { return hiz_pass_; }
  [[nodiscard]] BloomPass &bloom_pass() { return bloom_pass_; }
  [[nodiscard]] GtaoPass &gtao_pass() { return gtao_pass_; }
  [[nodiscard]] SsrPass &ssr_pass() { return ssr_pass_; }
  [[nodiscard]] VolumetricFogPass &volumetric_fog_pass() { return volumetric_fog_pass_; }
  [[nodiscard]] TonemapPass &tonemap_pass() { return tonemap_pass_; }
  [[nodiscard]] LightVizPass &light_heatmap_pass() { return light_heatmap_pass_; }
  [[nodiscard]] LightVizPass &light_grid_pass() { return light_grid_pass_; }
  [[nodiscard]] MeshletVizPass &meshlet_viz_pass() { return meshlet_viz_pass_; }
  [[nodiscard]] CullVizPass &cull_viz_pass() { return cull_viz_pass_; }
  [[nodiscard]] CullPass &cull_pass() { return cull_pass_; }
  [[nodiscard]] GridPass &grid_pass() { return grid_pass_; }
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
  void update_shadow_descriptors();
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
  void register_environment_passes();
  void register_depth_passes();
  void register_gpu_driven_passes();
  void register_color_passes();
  void register_post_process_passes();
  void register_debug_passes();
  void register_pass_resources();

  // ── GPU infrastructure ────────────────────────────────────────────────
  GpuContext gpu_;
  RenderPipeline render_pipeline_;
  RenderTargetPool target_pool_;
  FrameContext current_ctx_{};

  // ── Passes ────────────────────────────────────────────────────────────
  DepthPrePass depth_prepass_;
  ShadowPass shadow_pass_;
  DebugPass debug_pass_;
  GridPass grid_pass_;
  DepthVizPass depth_viz_pass_;
  WireframePass wireframe_pass_;
  NormalPass normal_pass_;
  OverdrawPass overdraw_pass_;
  UvVizPass uv_viz_pass_;
  BloomPass bloom_pass_;
  SkyPass sky_pass_;
  SkyCubemapPass sky_cubemap_pass_;
  TransparentPass transparent_pass_;
  TonemapPass tonemap_pass_;

  // Meshlet passes
  MeshShaderDepthPass mesh_shader_depth_pass_;
  MeshletVizPass meshlet_viz_pass_;
  CullPass cull_pass_;
  CullVizPass cull_viz_pass_;
  MeshletInstancePass meshlet_instance_pass_;
  HizPass hiz_pass_;

  // Post-process compute
  GtaoPass gtao_pass_;
  SsrPass ssr_pass_;
  VolumetricFogPass volumetric_fog_pass_;

  // Forward+ light culling + visualization
  LightCullPass light_cull_pass_;
  LightVizPass light_heatmap_pass_;
  LightVizPass light_grid_pass_;

  // Scene SDF
  SceneSdfPass scene_sdf_pass_;

  // Particles / sprites
  ParticleSystem particle_system_;
  SpriteRenderer sprite_renderer_;
  TilemapRenderer tilemap_renderer_;

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


  // ── Selection outline ─────────────────────────────────────────────────
  void draw_outline(VkCommandBuffer cmd, const FrameContext &ctx);

  // ── Editor / game state ───────────────────────────────────────────────
  std::unique_ptr<DebugDraw> debug_draw_;
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
  uint32_t active_sources_{~0u}; // all enabled by default
  AnimationSystem* animation_system_{nullptr};
  float ibl_intensity_{1.0f};
  float elapsed_time_{0.0f};

  // ── Scene viewports (render-to-texture for materials) ─────────────────
  std::unordered_map<std::string, std::unique_ptr<SceneViewport>> scene_viewports_;
};

} // namespace fjell
