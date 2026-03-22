#pragma once

#include "renderer/gpu_context.hpp"
#include "renderer/render_pipeline.hpp"
#include "renderer/scene_gatherer.hpp"
#include "renderer/debug_draw.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/particle_system.hpp"
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
#include "renderer/sprite_renderer.hpp"
#include "renderer/viewport_source.hpp"
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace fjell {

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

  void sync_sprite_textures(Scene &scene, ResourceManager &resources);

  /// Submit the previously recorded frame and present it.
  /// No-op on the first call (nothing recorded yet).
  void submit_and_present();

  /// Wait for the next frame slot, acquire swapchain image, record commands.
  void wait_and_record(const std::vector<ViewportRenderRequest> &viewports,
                       const struct DirectionalLight &light, Scene *scene,
                       float dt, uint32_t selected_id, ThreadPool &pool,
                       bool playing = false);

  void update_light_ssbo(uint32_t frame_index, Scene *scene);
  void wait_idle();
  void release_imgui_textures();
  void reload_pipeline();

  // ── Forwarding accessors to GpuContext ────────────────────────────────
  [[nodiscard]] GpuContext& gpu() { return gpu_; }
  [[nodiscard]] VkDevice device() const { return gpu_.device(); }
  [[nodiscard]] VkPhysicalDevice physical_device() const { return gpu_.physical_device(); }
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
  void set_use_mesh_shaders(bool enable) { gpu_.set_use_mesh_shaders(enable); }
  [[nodiscard]] MegaBuffer *mega_buffer() { return gpu_.mega_buffer(); }
  [[nodiscard]] MeshletBuffer *meshlet_buffer() { return gpu_.meshlet_buffer(); }
  [[nodiscard]] PipelineCache *pipeline_cache() { return gpu_.pipeline_cache(); }

  // ── Pass accessors (still owned by VulkanContext) ─────────────────────
  [[nodiscard]] DepthVizPass &depth_viz_pass() { return depth_viz_pass_; }
  [[nodiscard]] WireframePass &wireframe_pass() { return wireframe_pass_; }
  [[nodiscard]] NormalPass &normal_pass() { return normal_pass_; }
  [[nodiscard]] OverdrawPass &overdraw_pass() { return overdraw_pass_; }
  [[nodiscard]] UvVizPass &uv_viz_pass() { return uv_viz_pass_; }
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

  void set_show_colliders(bool show) { show_colliders_ = show; }
  [[nodiscard]] bool show_colliders() const { return show_colliders_; }
  void set_playing(bool playing) { playing_ = playing; }
  [[nodiscard]] bool is_playing() const { return playing_; }

  /// Initialize a GameUi instance using renderer resources.
  /// Ownership stays with the caller.
  void init_game_ui(class GameUi& ui, const std::string& font_path,
                    VkFormat output_format = VK_FORMAT_R8G8B8A8_SRGB);

  /// Callback for game UI rendering. Set by the game layer.
  /// Receives: cmd, global_set, bindless_set, extent, frame_index.
  using GameUiCallback = std::function<void(VkCommandBuffer, VkDescriptorSet,
                                            VkDescriptorSet, VkExtent2D, uint32_t)>;
  void set_game_ui_callback(GameUiCallback cb) { game_ui_callback_ = std::move(cb); }

  /// Set by ViewportManager each frame — top-left of the active viewport in
  /// window pixels.
  void set_viewport_offset(glm::vec2 offset) { viewport_offset_ = offset; }

  /// Bitmask of ViewportSource values currently displayed. Viz passes skip
  /// recording when their source bit is not set.
  void set_active_sources(uint32_t mask) { active_sources_ = mask; }

  void set_animation_system(class AnimationSystem* sys) { animation_system_ = sys; }

private:
  void
  record_command_buffer(VkCommandBuffer cmd, uint32_t image_index,
                        const std::vector<ViewportRenderRequest> &viewports,
                        const struct DirectionalLight &light, Scene *scene,
                        float dt, uint32_t selected_id, ThreadPool &pool,
                        bool playing);
  void render_scene_to(VkCommandBuffer cmd, OffscreenPass &target, Scene *scene,
                       float dt, uint32_t selected_id, ThreadPool &pool);
  void update_uniform_buffer(uint32_t frame_index, const glm::mat4 &view,
                             const glm::mat4 &projection,
                             const glm::vec3 &camera_position,
                             const struct DirectionalLight &light, Scene *scene,
                             VkExtent2D extent);

  void register_passes();

  // ── GPU infrastructure ────────────────────────────────────────────────
  GpuContext gpu_;
  RenderPipeline render_pipeline_;
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

  // Particles / sprites
  ParticleSystem particle_system_;
  SpriteRenderer sprite_renderer_;

  // ── Scene gathering ────────────────────────────────────────────────────
  SceneGatherer gatherer_;

  // ── Frame state ───────────────────────────────────────────────────────
  uint32_t current_frame_{MAX_FRAMES_IN_FLIGHT - 1};

  // Split-frame state: carried from wait_and_record to submit_and_present
  uint32_t recorded_frame_{0};
  uint32_t recorded_image_index_{0};
  bool has_recorded_frame_{false};

  // ── SSR prev-frame state ──────────────────────────────────────────────
  VkImage prev_frame_color_{VK_NULL_HANDLE};
  VkDeviceMemory prev_frame_color_memory_{VK_NULL_HANDLE};
  VkImageView prev_frame_color_view_{VK_NULL_HANDLE};
  VkExtent2D prev_frame_extent_{};

  // ── Selection outline ─────────────────────────────────────────────────
  VkPipelineLayout outline_layout_{VK_NULL_HANDLE};
  VkPipeline outline_pipeline_{VK_NULL_HANDLE};
  void create_outline_pipeline();
  void draw_outline(VkCommandBuffer cmd, const FrameContext &ctx);

  // ── Editor / game state ───────────────────────────────────────────────
  std::unique_ptr<DebugDraw> debug_draw_;
  OverlayCallback overlay_callback_;
  GameUiCallback game_ui_callback_;
  glm::vec2 viewport_offset_{0.0f, 0.0f};
  bool show_colliders_{false};
  bool playing_{false};
  uint32_t active_sources_{~0u}; // all enabled by default
  AnimationSystem* animation_system_{nullptr};
  float ibl_intensity_{1.0f};
};

} // namespace fjell
