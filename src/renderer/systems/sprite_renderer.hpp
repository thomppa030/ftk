#pragma once

#include "renderer/gpu/buffer.hpp"
#include "renderer/pass/render_pass.hpp"
#include "renderer/renderer_constants.hpp"

#include <glm/glm.hpp>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace fjell {

class Device;
class ResourceManager;
class Scene;

class SpriteRenderer {
public:
  void create(Device &device, const PassResources &resources);
  void destroy();

  // Resolve texture paths to bindless indices
  void sync_textures(Scene &scene, ResourceManager &resources,
                     const class Project *project = nullptr);

  // Gather sprites, sort back-to-front, upload SSBO
  void update(const Scene &scene, const glm::mat4 &view_projection,
              const glm::vec3 &camera_position, uint32_t frame_index,
              bool playing = false);

  // Record draw commands
  void record_render(const FrameContext &ctx);

private:
  struct GpuSprite {
    glm::vec3 position;     // 12
    uint32_t flags;         // 4  (bit 0: flat, bit 1: flip_h)
    glm::vec2 size;         // 8
    uint32_t texture_index; // 4
    float opacity;          // 4
    glm::vec4 tint;         // 16
    glm::vec4 uv_rect;     // 16 {u_min, v_min, u_max, v_max}
  };
  static_assert(sizeof(GpuSprite) == 64);

  VkDevice device_{VK_NULL_HANDLE};
  VmaAllocator allocator_{VK_NULL_HANDLE};

  // Pipeline
  VkDescriptorSetLayout sprite_set_layout_{VK_NULL_HANDLE};
  VkPipelineLayout layout_{VK_NULL_HANDLE};
  VkPipeline pipeline_{VK_NULL_HANDLE};

  // Double-buffered SSBOs
  std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> sprite_ssbos_;
  std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> sprite_sets_{};
  VkDescriptorPool pool_{VK_NULL_HANDLE};

  // Cached layouts from PassResources
  VkDescriptorSetLayout global_set_layout_{VK_NULL_HANDLE};
  VkDescriptorSetLayout bindless_set_layout_{VK_NULL_HANDLE};
  VkFormat color_format_{VK_FORMAT_UNDEFINED};
  VkFormat depth_format_{VK_FORMAT_UNDEFINED};
  VkSampleCountFlagBits sample_count_{VK_SAMPLE_COUNT_1_BIT};

  // Per-frame sprite count
  std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> sprite_counts_{};

  static constexpr uint32_t MAX_SPRITES = 4096;
  static constexpr VkDeviceSize SSBO_SIZE = MAX_SPRITES * sizeof(GpuSprite);

  // Reusable per-frame scratch (avoid heap allocs every frame)
  struct SortEntry {
    int32_t sort_layer;
    int32_t sort_order;
    float y_position;   // for top-down depth (lower Y = in front)
    float distance;     // for billboard distance sort
    uint32_t index;
    bool flat;
  };
  std::vector<GpuSprite> sprites_scratch_;
  std::vector<SortEntry> sort_scratch_;
};

} // namespace fjell
