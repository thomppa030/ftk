#pragma once

#include "gpu/binding.hpp"
#include "gpu/buffer.hpp"
#include "gpu/format.hpp"
#include "gpu/owned.hpp"
#include "gpu/pipeline.hpp"
#include "renderer/pass/render_pass.hpp"
#include "renderer/renderer_constants.hpp"
#include "ui/game_ui/text_shaper.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fjell {

namespace gpu {
class Device;
class RenderEncoder;
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

/// Last frame's mvp, for the velocity attachment. A uniform of each draw's
/// own rather than part of UiWorldPushConstants: that would put the struct
/// at 144 bytes, over the guaranteed-minimum 128 bytes of push data (this
/// engine isn't Linux-only, so it can't assume the larger limit desktop GPUs
/// happen to offer).
struct UiWorldFrameData {
    glm::mat4 prev_mvp;
};

class UiRenderer {
public:
    /// fjsl_pipelines compiles the `shader_type ui;` materials quads may
    /// name; null means materials are ignored, and it outlives this renderer.
    /// Screen UI draws to one target of `output_format`, the scene colour's
    /// when undefined.
    void create(const PassResources& resources, PipelineCache* fjsl_pipelines,
                gpu::Format output_format = gpu::Format::undefined);
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

    /// Draws the frame's quads and text into `pass`, a scope with one colour
    /// target of the output format and no depth, `extent` pixels large.
    void record(gpu::RenderEncoder& pass, gpu::BindGroup globals, gpu::BindGroup bindless,
                glm::uvec2 extent, uint32_t frame_index);

    /// Create worldspace pipelines (HDR format + scene MSAA). Call after create().
    void create_world_pipelines(const PassResources& resources);

    /// Draws the frame's quads and text into the scene's transparent pass.
    /// prev_mvp is last frame's mvp for this same widget (proj * view *
    /// world_transform, all from last frame) — used to compute the velocity
    /// attachment.
    void record_world(gpu::RenderEncoder& pass, gpu::BindGroup globals, gpu::BindGroup bindless,
                      glm::uvec2 extent, uint32_t frame_index, const UiWorldPushConstants& push,
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
        glm::vec4 params;        // opacity, unused, px_range, shadow blur (0 = the fill)
        glm::vec4 clip_rect;     // overflow:hidden scissor
        glm::vec2 viewport_size; // for px->NDC
        // Whole: a bindless index names a sampler in its top bits, past what
        // a float holds exactly.
        uint32_t atlas_index{0};
        uint32_t pad{0};
    };
    // Mirrored by `UiGlyph` in shaders/include/fjell_ui_common.glsl (std430).
    static_assert(sizeof(GpuUiGlyph) == 96);
    static_assert(offsetof(GpuUiGlyph, viewport_size) == 80);
    static_assert(offsetof(GpuUiGlyph, atlas_index) == 88);

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
    // compile generation moves (a hot-reload); the device keeps the old ones
    // until the frames using them are done.
    struct MaterialPipelines {
        gpu::Owned<gpu::GraphicsPipeline> screen;
        gpu::Owned<gpu::GraphicsPipeline> world;
        uint32_t generation{0};
    };
    // Builds or refreshes the pipelines for a PipelineCache entry. Returns
    // false when the entry has no usable ui fragment, in which case the quad
    // is drawn by the fixed fill instead.
    bool ensure_material_pipelines(uint32_t pipeline_id);
    std::unordered_map<uint32_t, MaterialPipelines> material_pipelines_;
    PipelineCache* fjsl_pipelines_{nullptr};

    // A screen UI pipeline: quads or glyphs blended over the one target in
    // the order they are drawn, no depth. Text reads no globals.
    [[nodiscard]] gpu::GraphicsPipelineDesc screen_desc(gpu::ShaderCode vertex, gpu::ShaderCode fragment,
                                                        std::string_view name, bool reads_globals) const;
    // A world UI pipeline: blended over the scene's four targets in the
    // transparent pass, tested against its depth and never writing it,
    // writing its own motion.
    [[nodiscard]] gpu::GraphicsPipelineDesc world_desc(gpu::ShaderCode vertex, gpu::ShaderCode fragment,
                                                       std::string_view name, bool reads_globals) const;
    [[nodiscard]] std::vector<gpu::SharedLayout> shared(bool reads_globals) const;
    [[nodiscard]] gpu::Owned<gpu::GraphicsPipeline> make(const gpu::GraphicsPipelineDesc& desc) const;

    gpu::Device* gpu_{nullptr};
    gpu::SharedLayout globals_{};
    gpu::SharedLayout bindless_{};
    gpu::Format output_format_{gpu::Format::undefined};
    // The scene targets world UI draws to, kept so material pipelines can
    // match them.
    bool has_world_pipelines_{false};
    PassResources world_{};

    gpu::Owned<gpu::GraphicsPipeline> quad_pipeline_;
    gpu::Owned<gpu::GraphicsPipeline> text_pipeline_;
    gpu::Owned<gpu::GraphicsPipeline> world_quad_pipeline_;
    gpu::Owned<gpu::GraphicsPipeline> world_text_pipeline_;

    // The frame slot's quads and glyphs, written by update() and
    // update_text() and read by its frame.
    std::array<gpu::Owned<gpu::Buffer>, MAX_FRAMES_IN_FLIGHT> quad_buffers_;
    std::array<gpu::Owned<gpu::Buffer>, MAX_FRAMES_IN_FLIGHT> glyph_buffers_;

    // Per-frame counts
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> quad_counts_{};
    // Screen pixels per layout pixel for each frame's quads. The shader
    // maps layout space onto the target through viewport_size; scissors are
    // set in target pixels and need the same factor.
    std::array<float, MAX_FRAMES_IN_FLIGHT> frame_scales_{};
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> text_counts_{};

    static constexpr uint32_t MAX_UI_QUADS = 4096;
    static constexpr uint32_t MAX_TEXT_GLYPHS = 8192;
    static constexpr uint64_t QUAD_BUFFER_SIZE = MAX_UI_QUADS * sizeof(GpuUiQuad);
    static constexpr uint64_t GLYPH_BUFFER_SIZE = MAX_TEXT_GLYPHS * sizeof(GpuUiGlyph);
};

} // namespace fjell
