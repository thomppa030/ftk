#pragma once

#include "renderer/gpu/buffer.hpp"
#include "renderer/pass/render_pass.hpp"
#include "renderer/renderer_constants.hpp"
#include "renderer/systems/render_subsystem.hpp"

#include <glm/glm.hpp>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>

namespace fjell {

class Device;
class Project;
class ResourceManager;
class Scene;

class TilemapRenderer : public RenderSubsystem {
public:
    void create(Device& device, const PassResources& resources) override;
    void destroy() override;

    // RenderSubsystem lifecycle
    void update(const RenderSubsystemContext& ctx) override;
    void record_opaque(const FrameContext& ctx) override;

    // Resolve texture paths to bindless indices (called externally by editor)
    void sync_textures(Scene& scene, ResourceManager& resources, const Project* project = nullptr);

private:
    void update_impl(const Scene& scene, const glm::mat4& view_projection, uint32_t frame_index);
    void record_render(const FrameContext& ctx);

    struct GpuTile {
        glm::vec2 position; // 8
        glm::vec2 size;     // 8
        glm::vec4 uv_rect;  // 16
    };
    static_assert(sizeof(GpuTile) == 32);

    struct PushConstants {
        uint32_t texture_index;
        float z_depth;
    };

    VkDevice device_{VK_NULL_HANDLE};
    VkPipelineCache vk_pipeline_cache_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};

    VkDescriptorSetLayout tile_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};

    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> tile_ssbos_;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> tile_sets_{};
    VkDescriptorPool pool_{VK_NULL_HANDLE};

    VkDescriptorSetLayout global_set_layout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout bindless_set_layout_{VK_NULL_HANDLE};
    VkFormat color_format_{VK_FORMAT_UNDEFINED};
    VkFormat depth_format_{VK_FORMAT_UNDEFINED};
    VkSampleCountFlagBits sample_count_{VK_SAMPLE_COUNT_1_BIT};

    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> tile_counts_{};
    std::array<PushConstants, MAX_FRAMES_IN_FLIGHT> push_constants_{};

    static constexpr uint32_t MAX_TILES = 16384;
    static constexpr VkDeviceSize SSBO_SIZE = MAX_TILES * sizeof(GpuTile);
};

} // namespace fjell
