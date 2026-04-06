#pragma once

#include "renderer/gpu/buffer.hpp"
#include "renderer/pass/render_pass.hpp"
#include "renderer/renderer_constants.hpp"
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

/// Push constants for worldspace UI rendering.
struct UiWorldPushConstants {
    glm::mat4 mvp;            // proj * view * world_transform (billboard baked in)
    float pixels_per_unit;     // UI pixels per world unit
    float viewport_width;      // virtual viewport width
    float viewport_height;     // virtual viewport height
    float _pad{0.0f};
};
static_assert(sizeof(UiWorldPushConstants) == 80);

class UiRenderer {
public:
    void create(Device& device, VmaAllocator allocator,
                const PassResources& resources,
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

    /// Create worldspace pipelines (HDR format + scene MSAA). Call after create().
    void create_world_pipelines(Device& device, const PassResources& resources);

    /// Record worldspace draw commands with push constants.
    void record_world(VkCommandBuffer cmd, VkDescriptorSet global_set, VkDescriptorSet bindless_set,
                      VkExtent2D extent, uint32_t frame_index, const UiWorldPushConstants& push);

private:
    struct GpuUiQuad {
        glm::vec4 rect;          // x, y, w, h (pixels)
        glm::vec4 color;         // background RGBA (or gradient start)
        glm::vec4 border_color;  // border RGBA
        glm::vec4 corners;       // border-radius (tl, tr, br, bl)
        glm::vec4 params;        // border_width, opacity, flags, gradient_angle
        glm::vec4 clip_rect;     // overflow:hidden scissor
        glm::vec4 gradient_to;   // gradient end color RGBA
        glm::vec2 viewport_size; // for px->NDC
        glm::vec2 shadow_params; // blur_radius, spread
        uint32_t texture_id;     // bindless texture index (0 = none)
        uint32_t material_id;    // material SSBO index (UINT32_MAX = none)
        float z_layer;           // depth ordering (0 = far, 1 = near)
        uint32_t quad_flags;     // bitfield: bit 0 = opaque
    };
    static_assert(sizeof(GpuUiQuad) == 144);

    struct GpuUiGlyph {
        glm::vec4 rect;          // x, y, w, h (pixels)
        glm::vec4 uv_rect;       // uv_x0, uv_y0, uv_x1, uv_y1
        glm::vec4 color;         // text color RGBA
        glm::vec4 params;        // opacity, atlas_index, px_range, reserved
        glm::vec4 clip_rect;     // overflow:hidden scissor
        glm::vec2 viewport_size; // for px->NDC
        glm::vec2 _pad;
    };
    static_assert(sizeof(GpuUiGlyph) == 96);

    void collect_quads(const UiNode& node, std::vector<GpuUiQuad>& quads,
                       float viewport_width, float viewport_height);

    // Scissor clip group: a contiguous range of quads sharing the same clip rect
    struct ClipGroup {
        glm::vec4 clip_rect;  // x, y, w, h in pixels
        uint32_t first_vertex;
        uint32_t vertex_count;
    };

    // Build contiguous clip groups from a range of quads
    static std::vector<ClipGroup> build_clip_groups(const GpuUiQuad* quads,
                                                     uint32_t first_quad,
                                                     uint32_t quad_count);

    // Per-frame clip groups for opaque and transparent passes
    std::array<std::vector<ClipGroup>, MAX_FRAMES_IN_FLIGHT> opaque_groups_;
    std::array<std::vector<ClipGroup>, MAX_FRAMES_IN_FLIGHT> transparent_groups_;

    VkDevice device_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};

    // Quad pipelines
    VkDescriptorSetLayout quad_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};           // transparent quads (blend ON, depth write OFF)
    VkPipeline opaque_pipeline_{VK_NULL_HANDLE};    // opaque quads (blend OFF, depth write ON)

    // Text pipeline (shares layout)
    VkPipeline text_pipeline_{VK_NULL_HANDLE};

    // Worldspace pipelines (HDR + scene MSAA, with push constants)
    VkPipelineLayout world_layout_{VK_NULL_HANDLE};
    VkPipeline world_opaque_pipeline_{VK_NULL_HANDLE};
    VkPipeline world_transparent_pipeline_{VK_NULL_HANDLE};
    VkPipeline world_text_pipeline_{VK_NULL_HANDLE};

    // Double-buffered SSBOs (quads)
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> quad_ssbos_;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> quad_sets_{};
    VkDescriptorPool pool_{VK_NULL_HANDLE};

    // Double-buffered SSBOs (text glyphs)
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> text_ssbos_;

    // Cached layouts from PassResources
    VkDescriptorSetLayout global_set_layout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout bindless_set_layout_{VK_NULL_HANDLE};
    VkFormat color_format_{VK_FORMAT_UNDEFINED};
    VkFormat depth_format_{VK_FORMAT_UNDEFINED};

    // Per-frame counts
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> quad_counts_{};
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> opaque_counts_{};
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> text_counts_{};

    static constexpr uint32_t MAX_UI_QUADS = 4096;
    static constexpr uint32_t MAX_TEXT_GLYPHS = 8192;
    static constexpr VkDeviceSize SSBO_SIZE = MAX_UI_QUADS * 144; // sizeof(GpuUiQuad)
    static constexpr VkDeviceSize TEXT_SSBO_SIZE = MAX_TEXT_GLYPHS * 96; // sizeof(GpuUiGlyph)
};

} // namespace fjell
