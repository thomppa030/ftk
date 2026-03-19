#pragma once

#include "renderer/buffer.hpp"
#include "renderer/pass/render_pass.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace fjell {

class Device;
struct UiNode;

class UiRenderer {
public:
    void create(Device& device, const PassResources& resources,
                VkFormat output_format = VK_FORMAT_UNDEFINED);
    void destroy();

    /// Flatten the UI node tree into GPU quads and upload to SSBO.
    void update(const UiNode& root, float viewport_width, float viewport_height,
                uint32_t frame_index);

    /// Record draw commands into the command buffer.
    void record(VkCommandBuffer cmd, VkDescriptorSet global_set, VkDescriptorSet bindless_set,
                VkExtent2D extent, uint32_t frame_index);

private:
    struct GpuUiQuad {
        glm::vec4 rect;          // x, y, w, h (pixels)
        glm::vec4 color;         // background RGBA
        glm::vec4 border_color;  // border RGBA
        glm::vec4 corners;       // border-radius (tl, tr, br, bl)
        glm::vec4 params;        // border_width, opacity, texture_index, flags
        glm::vec4 clip_rect;     // overflow:hidden scissor
        glm::vec2 viewport_size; // for px->NDC
        glm::vec2 _pad;          // align to 16 bytes
    };
    static_assert(sizeof(GpuUiQuad) == 112);

    void collect_quads(const UiNode& node, std::vector<GpuUiQuad>& quads,
                       float viewport_width, float viewport_height);

    VkDevice device_{VK_NULL_HANDLE};
    VkPhysicalDevice physical_device_{VK_NULL_HANDLE};

    // Pipeline
    VkDescriptorSetLayout quad_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};

    // Double-buffered SSBOs
    std::array<std::unique_ptr<Buffer>, 2> quad_ssbos_;
    std::array<VkDescriptorSet, 2> quad_sets_{};
    VkDescriptorPool pool_{VK_NULL_HANDLE};

    // Cached layouts from PassResources
    VkDescriptorSetLayout global_set_layout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout bindless_set_layout_{VK_NULL_HANDLE};
    VkFormat color_format_{VK_FORMAT_UNDEFINED};

    // Per-frame quad count
    std::array<uint32_t, 2> quad_counts_{};

    static constexpr uint32_t MAX_UI_QUADS = 4096;
    static constexpr VkDeviceSize SSBO_SIZE = MAX_UI_QUADS * sizeof(GpuUiQuad);
};

} // namespace fjell
