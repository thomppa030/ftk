#pragma once

#include "renderer/gpu/buffer.hpp"
#include "renderer/pass/render_pass.hpp"
#include "renderer/renderer_constants.hpp"
#include "ui/game_ui/text_shaper.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace fjell {

class Device;
namespace gpu {
class Device;
}
class PipelineCache;
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

/// Last frame's mvp, for the velocity attachment. Written to a tiny per-frame
/// buffer rather than folded into UiWorldPushConstants: that would put the
/// struct at 144 bytes, over Vulkan's guaranteed-minimum 128-byte push
/// constant range (this engine isn't Linux-only, so it can't assume the
/// larger limit desktop GPUs happen to offer).
struct UiWorldFrameData {
    glm::mat4 prev_mvp;
};

class UiRenderer {
public:
    /// fjsl_pipelines compiles the `shader_type ui;` materials quads may
    /// name; null means materials are ignored. Pipelines built from it wait
    /// on `gpu` for the frames using them, and it outlives this renderer.
    void create(Device& device, VmaAllocator allocator,
                const PassResources& resources,
                PipelineCache* fjsl_pipelines, gpu::Device& gpu,
                VkFormat output_format = VK_FORMAT_UNDEFINED);
    void destroy();

    /// Flatten the UI node tree into GPU quads and upload to SSBO.
    /// viewport_width/height are the layout space the quads are positioned
    /// in; frame_scale is how many target pixels one layout pixel covers.
    void update(const UiNode& root, float viewport_width, float viewport_height,
                float frame_scale, uint32_t frame_index);

    /// Adds the glyphs shaped for `text_node`, placed in `layout_node`'s box
    /// (the element the text belongs to), to this frame's text. They paint
    /// where the text node sits in the tree.
    void update_text(const std::vector<PositionedGlyph>& glyphs,
                     const UiNode& text_node, const UiNode& layout_node,
                     uint32_t atlas_index, float pixel_range,
                     float viewport_width, float viewport_height,
                     uint32_t frame_index);

    /// Puts this frame's quads and text in the order record() and
    /// record_world() paint them: tree order, so a node covers what came
    /// before it, text included, and its children cover it. Call once
    /// update() and every update_text() of the frame are done.
    void build_runs(uint32_t frame_index);

    /// Record draw commands into the command buffer (quads + text).
    void record(VkCommandBuffer cmd, VkDescriptorSet global_set, VkDescriptorSet bindless_set,
                VkExtent2D extent, uint32_t frame_index);

    /// Create worldspace pipelines (HDR format + scene MSAA). Call after create().
    void create_world_pipelines(Device& device, const PassResources& resources);

    /// Record worldspace draw commands with push constants. prev_mvp is last
    /// frame's mvp for this same widget (proj * view * world_transform, all
    /// from last frame) — used to compute the velocity attachment.
    void record_world(VkCommandBuffer cmd, VkDescriptorSet global_set, VkDescriptorSet bindless_set,
                      VkExtent2D extent, uint32_t frame_index, const UiWorldPushConstants& push,
                      const glm::mat4& prev_mvp);

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
        uint32_t pad0{0};        // keep `state` on 16 bytes, where std430 puts it
        uint32_t pad1{0};
        glm::vec4 state;         // FILL, HOVER, ACTIVE, FOCUS for shader_type ui
        glm::vec4 bar;           // FILL_TARGET, FILL_VELOCITY, FILL_AGITATION, unused
    };
    // Mirrored by `UiQuad` in shaders/include/fjell_ui_common.glsl (std430).
    static_assert(sizeof(GpuUiQuad) == 176);
    static_assert(offsetof(GpuUiQuad, viewport_size) == 112);
    static_assert(offsetof(GpuUiQuad, shadow_params) == 120);
    static_assert(offsetof(GpuUiQuad, texture_id) == 128);
    static_assert(offsetof(GpuUiQuad, material_id) == 132);
    static_assert(offsetof(GpuUiQuad, state) == 144);
    static_assert(offsetof(GpuUiQuad, bar) == 160);

    struct GpuUiGlyph {
        glm::vec4 rect;          // x, y, w, h (pixels)
        glm::vec4 uv_rect;       // uv_x0, uv_y0, uv_x1, uv_y1
        glm::vec4 color;         // text color RGBA
        glm::vec4 params;        // opacity, atlas_index, px_range, shadow blur (0 = the fill)
        glm::vec4 clip_rect;     // overflow:hidden scissor
        glm::vec2 viewport_size; // for px->NDC
        glm::vec2 _pad;
    };
    static_assert(sizeof(GpuUiGlyph) == 96);

    // A quad as collected from the tree, with what the CPU side needs to
    // order and batch it and the GPU side never reads.
    struct CollectedQuad {
        GpuUiQuad gpu;
        uint32_t pipeline_id{UINT32_MAX}; // PipelineCache entry of a ui material, else none
    };

    // What the UI paints, in tree order: a quad (its index in the frame's
    // quads), or the text of a text node, whose glyphs update_text() adds.
    struct PaintItem {
        const UiNode* text_node{nullptr};  // null for a quad
        uint32_t quad{0};
    };

    void collect_quads(const UiNode& node, std::vector<CollectedQuad>& quads,
                       std::vector<PaintItem>& items,
                       float viewport_width, float viewport_height);

    // A run painted in one draw: quads sharing a pipeline and a scissor, or
    // glyphs, which clip in their own shader.
    struct PaintRun {
        bool text{false};
        glm::vec4 clip_rect{0.0f};         // quads: x, y, w, h in layout pixels
        uint32_t pipeline_id{UINT32_MAX};  // quads: ui material pipeline, or none for the fixed fill
        uint32_t first{0};                 // first quad or glyph
        uint32_t count{0};
    };

    // The glyphs one text node added this frame.
    struct GlyphRange {
        uint32_t first{0};
        uint32_t count{0};
    };

    // Per frame: the quads as collected, what paints in which order, the
    // glyphs of each text node, and the runs build_runs() made of them.
    std::array<std::vector<CollectedQuad>, MAX_FRAMES_IN_FLIGHT> collected_;
    std::array<std::vector<PaintItem>, MAX_FRAMES_IN_FLIGHT> paint_items_;
    std::array<std::unordered_map<const UiNode*, GlyphRange>, MAX_FRAMES_IN_FLIGHT> glyph_ranges_;
    std::array<std::vector<PaintRun>, MAX_FRAMES_IN_FLIGHT> runs_;

    // Pipelines built for one `shader_type ui;` PipelineCache entry, in the
    // two shapes the fixed quad pipelines come in. Rebuilt when the entry's
    // compile generation moves (a hot-reload), old ones kept until the frames
    // using them are done.
    struct MaterialPipelines {
        VkPipeline screen{VK_NULL_HANDLE};
        VkPipeline world{VK_NULL_HANDLE};
        uint32_t generation{0};
    };
    // Builds or refreshes the pipelines for a PipelineCache entry. Returns
    // false when the entry has no usable ui fragment, in which case the quad
    // is drawn by the fixed fill instead.
    bool ensure_material_pipelines(uint32_t pipeline_id);
    void retire_material_pipelines(MaterialPipelines& mp);
    std::unordered_map<uint32_t, MaterialPipelines> material_pipelines_;
    PipelineCache* fjsl_pipelines_{nullptr};
    gpu::Device* gpu_device_{nullptr};
    // Worldspace pipeline shape, kept so material pipelines can match it
    bool has_world_pipelines_{false};
    VkSampleCountFlagBits world_samples_{VK_SAMPLE_COUNT_1_BIT};
    VkFormat world_color_format_{VK_FORMAT_UNDEFINED};

    VkDevice device_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};

    // Quad pipelines
    VkDescriptorSetLayout quad_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};           // quads, blended

    // Text pipeline (shares layout)
    VkPipeline text_pipeline_{VK_NULL_HANDLE};

    // Worldspace pipelines (HDR + scene MSAA, with push constants)
    VkPipelineLayout world_layout_{VK_NULL_HANDLE};
    VkPipeline world_transparent_pipeline_{VK_NULL_HANDLE};
    VkPipeline world_text_pipeline_{VK_NULL_HANDLE};

    // Worldspace's own set 2 layout/pool (see create_world_pipelines) and its
    // per-frame prev_mvp UBO for the velocity attachment.
    VkDescriptorSetLayout world_set_layout_{VK_NULL_HANDLE};
    VkDescriptorPool world_pool_{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> world_sets_{};
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> world_frame_buffers_;

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
    // The scene's depth, which world UI tests against and never writes.
    // Screen UI has none: it paints in tree order.
    VkFormat depth_format_{VK_FORMAT_UNDEFINED};

    // Per-frame counts
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> quad_counts_{};
    // Screen pixels per layout pixel for each frame's quads. The shader
    // maps layout space onto the target through viewport_size; scissors are
    // set in target pixels and need the same factor.
    std::array<float, MAX_FRAMES_IN_FLIGHT> frame_scales_{};
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> text_counts_{};

    static constexpr uint32_t MAX_UI_QUADS = 4096;
    static constexpr uint32_t MAX_TEXT_GLYPHS = 8192;
    static constexpr VkDeviceSize SSBO_SIZE = MAX_UI_QUADS * sizeof(GpuUiQuad);
    static constexpr VkDeviceSize TEXT_SSBO_SIZE = MAX_TEXT_GLYPHS * sizeof(GpuUiGlyph);
};

} // namespace fjell
