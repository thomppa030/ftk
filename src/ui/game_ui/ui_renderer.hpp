#pragma once

#include "renderer/buffer.hpp"
#include "renderer/pass/render_pass.hpp"
#include "ui/game_ui/text_shaper.hpp"

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

    /// Upload shaped text glyphs to the text SSBO.
    void update_text(const std::vector<PositionedGlyph>& glyphs,
                     const UiNode& source_node,
                     uint32_t atlas_index, float pixel_range,
                     float viewport_width, float viewport_height,
                     uint32_t frame_index);

    /// Record draw commands into the command buffer (quads + text).
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

    struct GpuUiGlyph {
        glm::vec4 rect;          // x, y, w, h (pixels)
        glm::vec4 uv_rect;       // uv_x0, uv_y0, uv_x1, uv_y1
        glm::vec4 color;         // text color RGBA
        glm::vec4 params;        // opacity, atlas_index, font_size, reserved
        glm::vec4 clip_rect;     // overflow:hidden scissor
        glm::vec2 viewport_size; // for px->NDC
        glm::vec2 _pad;
    };
    static_assert(sizeof(GpuUiGlyph) == 96);

    void collect_quads(const UiNode& node, std::vector<GpuUiQuad>& quads,
                       float viewport_width, float viewport_height);

    VkDevice device_{VK_NULL_HANDLE};
    VkPhysicalDevice physical_device_{VK_NULL_HANDLE};

    // Quad pipeline
    VkDescriptorSetLayout quad_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};

    // Text pipeline (shares layout)
    VkPipeline text_pipeline_{VK_NULL_HANDLE};

    // Double-buffered SSBOs (quads)
    std::array<std::unique_ptr<Buffer>, 2> quad_ssbos_;
    std::array<VkDescriptorSet, 2> quad_sets_{};
    VkDescriptorPool pool_{VK_NULL_HANDLE};

    // Double-buffered SSBOs (text glyphs)
    std::array<std::unique_ptr<Buffer>, 2> text_ssbos_;

    // Cached layouts from PassResources
    VkDescriptorSetLayout global_set_layout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout bindless_set_layout_{VK_NULL_HANDLE};
    VkFormat color_format_{VK_FORMAT_UNDEFINED};

    // Per-frame counts
    std::array<uint32_t, 2> quad_counts_{};
    std::array<uint32_t, 2> text_counts_{};

    static constexpr uint32_t MAX_UI_QUADS = 4096;
    static constexpr uint32_t MAX_TEXT_GLYPHS = 8192;
    static constexpr VkDeviceSize SSBO_SIZE = MAX_UI_QUADS * sizeof(GpuUiQuad);
    static constexpr VkDeviceSize TEXT_SSBO_SIZE = MAX_TEXT_GLYPHS * sizeof(GpuUiGlyph);
};

} // namespace fjell
