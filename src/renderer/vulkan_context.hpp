#pragma once

#include "renderer/bindless.hpp"
#include "renderer/buffer.hpp"
#include "renderer/debug_draw.hpp"
#include "renderer/descriptor.hpp"
#include "renderer/device.hpp"
#include "renderer/draw_data_descriptor.hpp"
#include "renderer/indirect_batch.hpp"
#include "renderer/mega_buffer.hpp"
#include "renderer/meshlet_buffer.hpp"
#include "renderer/meshlet_descriptor.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/particle_system.hpp"
#include "renderer/pass/bloom_pass.hpp"
#include "renderer/pass/cull_pass.hpp"
#include "renderer/pass/cull_viz_pass.hpp"
#include "renderer/pass/debug_pass.hpp"
#include "renderer/pass/depth_prepass.hpp"
#include "renderer/pass/depth_viz_pass.hpp"
#include "renderer/pass/grid_pass.hpp"
#include "renderer/pass/gtao_pass.hpp"
#include "renderer/pass/light_cull_pass.hpp"
#include "renderer/pass/light_viz_pass.hpp"
#include "renderer/pass/mesh_shader_depth_pass.hpp"
#include "renderer/pass/mesh_shader_pass.hpp"
#include "renderer/pass/meshlet_viz_pass.hpp"
#include "renderer/pass/normal_pass.hpp"
#include "renderer/pass/overdraw_pass.hpp"
#include "renderer/pass/shadow_pass.hpp"
#include "renderer/pass/sky_pass.hpp"
#include "renderer/pass/tonemap_pass.hpp"
#include "renderer/pass/transparent_pass.hpp"
#include "renderer/pass/uv_viz_pass.hpp"
#include "renderer/pass/wireframe_pass.hpp"
#include "renderer/pipeline.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/render_queue.hpp"
#include "renderer/renderer_constants.hpp"
#include "renderer/sprite_renderer.hpp"
#include "renderer/swapchain.hpp"
#include "renderer/thread_command_pools.hpp"
#include "renderer/viewport_source.hpp"
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#ifdef FJELL_ENABLE_TRACY
#include <tracy/TracyVulkan.hpp>
#endif

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace fjell {

class Window;
class Scene;
class Input;
class ResourceManager;
class ThreadPool;
struct RenderStats;

class VulkanContext {
public:
  explicit VulkanContext(Window &window);
  ~VulkanContext();

  VulkanContext(const VulkanContext &) = delete;
  VulkanContext &operator=(const VulkanContext &) = delete;
  VulkanContext(VulkanContext &&) = delete;
  VulkanContext &operator=(VulkanContext &&) = delete;

  void sync_sprite_textures(Scene &scene, ResourceManager &resources);
  void draw_frame(const std::vector<ViewportRenderRequest> &viewports,
                  const struct DirectionalLight &light, Scene *scene, float dt,
                  uint32_t selected_id, ThreadPool &pool,
                  bool playing = false);
  void update_light_ssbo(uint32_t frame_index, Scene *scene);
  void wait_idle();
  void reload_pipeline();

  [[nodiscard]] VkDevice device() const { return device_.handle(); }
  [[nodiscard]] VkPhysicalDevice physical_device() const {
    return device_.physical_device();
  }
  [[nodiscard]] VkCommandPool command_pool() const { return command_pool_; }
  [[nodiscard]] VkQueue graphics_queue() const {
    return device_.graphics_queue();
  }
  [[nodiscard]] BindlessTextures &bindless_textures() {
    return bindless_textures_;
  }
  [[nodiscard]] VkInstance instance() const { return device_.instance(); }
  [[nodiscard]] VkFormat swapchain_format() const {
    return swapchain_.format();
  }
  [[nodiscard]] VkExtent2D swapchain_extent() const {
    return swapchain_.extent();
  }
  [[nodiscard]] uint32_t image_count() const {
    return swapchain_.image_count();
  }
  [[nodiscard]] uint32_t graphics_family() const;
  [[nodiscard]] const std::string &gpu_name() const {
    return device_.gpu_name();
  }
  [[nodiscard]] RenderStats &render_stats() { return *render_stats_; }
  [[nodiscard]] Device &device_ref() { return device_; }
  [[nodiscard]] VkSampleCountFlagBits msaa_samples() const {
    return msaa_samples_;
  }
  [[nodiscard]] bool mesh_shaders_available() const {
    return device_.mesh_shader_supported();
  }
  [[nodiscard]] bool use_mesh_shaders() const { return use_mesh_shaders_; }
  void set_use_mesh_shaders(bool enable) {
    use_mesh_shaders_ = enable && device_.mesh_shader_supported();
  }
  [[nodiscard]] DepthVizPass &depth_viz_pass() { return depth_viz_pass_; }
  [[nodiscard]] WireframePass &wireframe_pass() { return wireframe_pass_; }
  [[nodiscard]] NormalPass &normal_pass() { return normal_pass_; }
  [[nodiscard]] OverdrawPass &overdraw_pass() { return overdraw_pass_; }
  [[nodiscard]] UvVizPass &uv_viz_pass() { return uv_viz_pass_; }
  [[nodiscard]] BloomPass &bloom_pass() { return bloom_pass_; }
  [[nodiscard]] GtaoPass &gtao_pass() { return gtao_pass_; }
  [[nodiscard]] TonemapPass &tonemap_pass() { return tonemap_pass_; }
  [[nodiscard]] LightVizPass &light_heatmap_pass() {
    return light_heatmap_pass_;
  }
  [[nodiscard]] LightVizPass &light_grid_pass() { return light_grid_pass_; }
  [[nodiscard]] MeshletVizPass &meshlet_viz_pass() { return meshlet_viz_pass_; }
  [[nodiscard]] CullVizPass &cull_viz_pass() { return cull_viz_pass_; }
  [[nodiscard]] CullPass &cull_pass() { return cull_pass_; }
  using OverlayCallback = std::function<void(VkCommandBuffer)>;
  void set_overlay_callback(OverlayCallback callback) {
    overlay_callback_ = std::move(callback);
  }

  void set_show_colliders(bool show) { show_colliders_ = show; }
  [[nodiscard]] bool show_colliders() const { return show_colliders_; }
  void set_playing(bool playing) { playing_ = playing; }
  [[nodiscard]] bool is_playing() const { return playing_; }
  [[nodiscard]] GridPass &grid_pass() { return grid_pass_; }
  [[nodiscard]] DebugDraw &debug_draw() { return *debug_draw_; }
  [[nodiscard]] MegaBuffer *mega_buffer() { return mega_buffer_.get(); }
  [[nodiscard]] MeshletBuffer *meshlet_buffer() {
    return meshlet_buffer_.get();
  }
  [[nodiscard]] PipelineCache *pipeline_cache() {
    return pipeline_cache_.get();
  }
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

private:
  void create_descriptor_resources();
  void create_command_pool();
  void create_command_buffers();
  void create_sync_objects();

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

  static void
  transition_image(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout,
                   VkImageLayout new_layout, VkPipelineStageFlags2 src_stage,
                   VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                   VkAccessFlags2 dst_access,
                   VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);

  Window &window_;
  Device device_;
  Swapchain swapchain_;

  VkCommandPool command_pool_{VK_NULL_HANDLE};
  ThreadCommandPools secondary_pools_;
  bool secondary_pools_initialized_{false};

  // Descriptors
  std::unique_ptr<DescriptorSetLayout> global_set_layout_;
  std::unique_ptr<DescriptorPool> global_pool_;
  std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> global_descriptor_sets_{};
  std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> uniform_buffers_;

  BindlessTextures bindless_textures_;

  // Pipelines / passes
  std::unique_ptr<Pipeline> pipeline_;
  std::unique_ptr<PipelineCache> pipeline_cache_;
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
  TransparentPass transparent_pass_;
  TonemapPass tonemap_pass_;

  // Command buffers and sync
  std::vector<VkCommandBuffer> command_buffers_;

  OverlayCallback overlay_callback_;

  std::array<VkSemaphore, MAX_FRAMES_IN_FLIGHT> image_available_{};
  std::array<VkFence, MAX_FRAMES_IN_FLIGHT> in_flight_{};
  uint32_t current_frame_{0};

  std::unique_ptr<RenderStats> render_stats_;
  RenderQueue render_queue_;
  glm::vec3 camera_position_{0.0f};
  glm::mat4 view_matrix_{1.0f};
  glm::mat4 projection_matrix_{1.0f};
  VkSampleCountFlagBits msaa_samples_{VK_SAMPLE_COUNT_1_BIT};

  // Multi-draw indirect
  std::unique_ptr<MegaBuffer> mega_buffer_;
  std::unique_ptr<IndirectBatch> indirect_batch_;
  DrawDataDescriptor draw_data_descriptor_;

  // Shadow-specific batch (built from all shadow casters, not camera-culled)
  std::unique_ptr<IndirectBatch> shadow_batch_;
  DrawDataDescriptor shadow_draw_data_;

  // Meshlet GPU buffers + descriptor + passes
  std::unique_ptr<MeshletBuffer> meshlet_buffer_;
  MeshletDescriptor meshlet_descriptor_;
  MeshShaderDepthPass mesh_shader_depth_pass_;
  MeshShaderPass mesh_shader_pass_;
  MeshletVizPass meshlet_viz_pass_;
  CullPass cull_pass_;
  CullVizPass cull_viz_pass_;
  bool use_mesh_shaders_{false};

  // GT-VBAO ambient occlusion
  GtaoPass gtao_pass_;

  // Forward+ light culling + visualization
  LightCullPass light_cull_pass_;
  LightVizPass light_heatmap_pass_;
  LightVizPass light_grid_pass_;
  std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> light_ssbo_;
  std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> tile_light_ssbo_;
  VkSampler depth_sampler_{VK_NULL_HANDLE};

  // Particles
  ParticleSystem particle_system_;

  // Sprites
  SpriteRenderer sprite_renderer_;

  // Game UI callback (owned by game layer)
  GameUiCallback game_ui_callback_;
  glm::vec2 viewport_offset_{0.0f, 0.0f};

  // Selection outline
  VkPipelineLayout outline_layout_{VK_NULL_HANDLE};
  VkPipeline outline_pipeline_{VK_NULL_HANDLE};
  void create_outline_pipeline();
  void draw_outline(VkCommandBuffer cmd, const FrameContext &ctx);

  // Debug collider visualization
  std::unique_ptr<DebugDraw> debug_draw_;
  bool show_colliders_{true};
  bool playing_{false};
  uint32_t active_sources_{~0u}; // all enabled by default

#ifdef FJELL_ENABLE_TRACY
  tracy::VkCtx* tracy_vk_ctx_{nullptr};
#endif
};

} // namespace fjell
