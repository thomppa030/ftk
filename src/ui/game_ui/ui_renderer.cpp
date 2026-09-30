#include "ui/game_ui/ui_renderer.hpp"
#include "ui/game_ui/ui_node.hpp"
#include "gpu/device.hpp"
#include "gpu/render_encoder.hpp"
#include "gpu/shader.hpp"
#include "renderer/pipeline_cache.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace fjell {

namespace {

constexpr std::string_view QUAD_BUFFER = "UiQuadBuffer";
constexpr std::string_view GLYPH_BUFFER = "UiGlyphBuffer";
constexpr std::string_view WORLD_FRAME = "WorldUiFrameData";

} // namespace

std::vector<gpu::SharedLayout> UiRenderer::shared(bool reads_globals) const {
    if (reads_globals) return {globals_, bindless_};
    return {bindless_};
}

gpu::GraphicsPipelineDesc UiRenderer::screen_desc(gpu::ShaderCode vertex, gpu::ShaderCode fragment,
                                                  std::string_view name, bool reads_globals) const {
    return {
        .vertex = vertex,
        .fragment = fragment,
        .color = {{output_format_, gpu::Blend::alpha}},
        .shared = shared(reads_globals),
        .name = name,
    };
}

gpu::GraphicsPipelineDesc UiRenderer::world_desc(gpu::ShaderCode vertex, gpu::ShaderCode fragment,
                                                 std::string_view name, bool reads_globals) const {
    // The UI has no surface to give metallic/roughness or normal; the
    // worldspace fragment shaders compute a real per-pixel motion.
    return {
        .vertex = vertex,
        .fragment = fragment,
        .depth = {.test = true, .write = false, .compare = gpu::Compare::less_equal},
        .color = {{world_.scene_color_format, gpu::Blend::alpha},
                  {world_.scene_mr_format, gpu::Blend::none, gpu::ChannelMask{}},
                  {world_.scene_normal_format, gpu::Blend::none, gpu::ChannelMask{}},
                  {world_.scene_velocity_format, gpu::Blend::none, gpu::Channel::r | gpu::Channel::g}},
        .depth_format = world_.scene_depth_format,
        .samples = world_.samples,
        .shared = shared(reads_globals),
        .name = name,
    };
}

gpu::Owned<gpu::GraphicsPipeline> UiRenderer::make(const gpu::GraphicsPipelineDesc& desc) const {
    auto made = gpu_->create(desc);
    if (!made) throw std::runtime_error("UI: " + made.error());
    return std::move(*made);
}

void UiRenderer::create(const PassResources& resources, PipelineCache* fjsl_pipelines,
                        gpu::Format output_format) {
    gpu_ = resources.device;
    fjsl_pipelines_ = fjsl_pipelines;
    globals_ = resources.globals;
    bindless_ = resources.bindless;
    output_format_ = output_format != gpu::Format::undefined ? output_format : resources.scene_color_format;

    // Screen UI draws into a scope with no depth: it paints in tree order.
    quad_pipeline_ = make(screen_desc("shaders/ui_quad.vert", "shaders/ui_quad.frag", "ui_quad", true));
    text_pipeline_ = make(screen_desc("shaders/ui_text.vert", "shaders/ui_text.frag", "ui_text", false));

    auto buffer = [&](uint64_t size, std::string_view name) {
        auto made = gpu_->create(gpu::BufferDesc{
            .size = size, .use = gpu::BufferUse::storage, .memory = gpu::Memory::upload, .name = name});
        if (!made) throw std::runtime_error("UI: " + made.error());
        return std::move(*made);
    };
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        quad_buffers_[i] = buffer(QUAD_BUFFER_SIZE, "ui_quads");
        glyph_buffers_[i] = buffer(GLYPH_BUFFER_SIZE, "ui_glyphs");
    }

    FJELL_GFX_DEBUG("UI renderer created (quad + text pipelines)");
}

void UiRenderer::destroy() {
    for (auto& buffer : quad_buffers_) buffer.reset();
    for (auto& buffer : glyph_buffers_) buffer.reset();
    material_pipelines_.clear();
    quad_pipeline_.reset();
    text_pipeline_.reset();
    world_quad_pipeline_.reset();
    world_text_pipeline_.reset();
}

void UiRenderer::update(const UiNode& root, float viewport_width, float viewport_height,
                         float frame_scale, uint32_t frame_index) {
    FJELL_PROFILE_SCOPE_N("ui_renderer_update");
    auto& quads = collected_[frame_index];
    auto& items = paint_items_[frame_index];
    quads.clear();
    items.clear();
    collect_quads(root, quads, items, viewport_width, viewport_height);

    // A material quad needs its pipeline; one whose shader failed to compile
    // falls back to the fixed fill so the node still shows its colour.
    for (auto& q : quads) {
        if (q.pipeline_id != UINT32_MAX && !ensure_material_pipelines(q.pipeline_id)) {
            q.pipeline_id = UINT32_MAX;
            q.gpu.material_id = UINT32_MAX;
            q.gpu.params.z = 0.0f;
        }
    }

    uint32_t count = std::min(static_cast<uint32_t>(quads.size()), MAX_UI_QUADS);
    quad_counts_[frame_index] = count;
    frame_scales_[frame_index] = frame_scale;
    if (count > 0) {
        auto* dst = reinterpret_cast<GpuUiQuad*>(gpu_->mapped(quad_buffers_[frame_index]).data());
        for (uint32_t i = 0; i < count; ++i) {
            dst[i] = quads[i].gpu;
        }
    }

    // Filled by the update_text() calls that follow.
    text_counts_[frame_index] = 0;
    glyph_ranges_[frame_index].clear();
    runs_[frame_index].clear();
}

void UiRenderer::build_runs(uint32_t frame_index) {
    const auto& quads = collected_[frame_index];
    const uint32_t quad_count = quad_counts_[frame_index];
    auto& ranges = glyph_ranges_[frame_index];
    auto& runs = runs_[frame_index];
    runs.clear();

    auto add_glyphs = [&](GlyphRange range) {
        if (range.count == 0) return;
        if (!runs.empty() && runs.back().text && runs.back().first + runs.back().count == range.first) {
            runs.back().count += range.count;
            return;
        }
        runs.push_back({.text = true, .first = range.first, .count = range.count});
    };

    for (const PaintItem& item : paint_items_[frame_index]) {
        if (item.text_node != nullptr) {
            if (auto it = ranges.find(item.text_node); it != ranges.end()) {
                add_glyphs(it->second);
                ranges.erase(it);
            }
            continue;
        }
        // Past the quad buffer's end: the rest of the quads were dropped.
        if (item.quad >= quad_count) continue;
        const CollectedQuad& q = quads[item.quad];
        PaintRun* last = runs.empty() ? nullptr : &runs.back();
        if (last != nullptr && !last->text && last->pipeline_id == q.pipeline_id &&
            last->clip_rect == q.gpu.clip_rect && last->first + last->count == item.quad) {
            ++last->count;
        } else {
            runs.push_back({.clip_rect = q.gpu.clip_rect, .pipeline_id = q.pipeline_id,
                            .first = item.quad, .count = 1});
        }
    }

    // Text whose node the quads never reached (inside a box laid out with no
    // size) still draws, last, in the order it was shaped.
    std::vector<GlyphRange> unplaced;
    unplaced.reserve(ranges.size());
    for (const auto& [node, range] : ranges) unplaced.push_back(range);
    std::sort(unplaced.begin(), unplaced.end(),
              [](const GlyphRange& a, const GlyphRange& b) { return a.first < b.first; });
    for (const GlyphRange& range : unplaced) add_glyphs(range);
    ranges.clear();
}

void UiRenderer::update_text(const std::vector<PositionedGlyph>& glyphs,
                              const UiNode& text_node, const UiNode& source_node,
                              uint32_t atlas_index, float pixel_range,
                              float viewport_width, float viewport_height,
                              uint32_t frame_index) {
    if (glyphs.empty()) return;
    const uint32_t first = text_counts_[frame_index];

    auto* base = reinterpret_cast<GpuUiGlyph*>(gpu_->mapped(glyph_buffers_[frame_index]).data());
    const auto& style = source_node.style;

    // One instance per glyph, moved by the offset and drawn in the colour
    // given; a blur of zero is the glyph's own fill. A blurred shadow
    // reaches past the glyph by its blur, so its quad grows by that margin
    // and its atlas coordinates run on past the glyph's cell at the same
    // scale; the shader keeps its samples inside the cell.
    auto emit = [&](UiColor color, float offset_x, float offset_y, float blur) {
        uint32_t existing = text_counts_[frame_index];
        uint32_t count = std::min(static_cast<uint32_t>(glyphs.size()),
                                  MAX_TEXT_GLYPHS - existing);
        GpuUiGlyph* dst = base + existing;
        for (uint32_t i = 0; i < count; ++i) {
            const auto& pg = glyphs[i];
            float du = pg.w > 0.0f ? (pg.uv_x1 - pg.uv_x0) / pg.w * blur : 0.0f;
            float dv = pg.h > 0.0f ? (pg.uv_y1 - pg.uv_y0) / pg.h * blur : 0.0f;
            GpuUiGlyph gg{};
            gg.rect = {source_node.layout_rect.x + pg.x + offset_x - blur,
                       source_node.layout_rect.y + pg.y + offset_y - blur,
                       pg.w + 2.0f * blur, pg.h + 2.0f * blur};
            gg.uv_rect = {pg.uv_x0 - du, pg.uv_y0 - dv, pg.uv_x1 + du, pg.uv_y1 + dv};
            gg.color = {color.r, color.g, color.b, color.a};
            gg.params = {source_node.draw_opacity, 0.0f, pixel_range, blur};
            gg.clip_rect = {source_node.clip_rect.x, source_node.clip_rect.y,
                            source_node.clip_rect.w, source_node.clip_rect.h};
            gg.viewport_size = {viewport_width, viewport_height};
            gg.atlas_index = atlas_index;
            dst[i] = gg;
        }
        text_counts_[frame_index] = existing + count;
    };

    // Glyphs draw in buffer order, so every shadow of the node goes down
    // before any of its fills and none lands on a neighbouring letter.
    const auto& shadow = style.text_shadow;
    if (shadow.active()) emit(shadow.color, shadow.offset_x, shadow.offset_y, shadow.blur);
    emit(style.color, 0.0f, 0.0f, 0.0f);

    glyph_ranges_[frame_index][&text_node] = {first, text_counts_[frame_index] - first};
}

void UiRenderer::collect_quads(const UiNode& node, std::vector<CollectedQuad>& quads,
                                std::vector<PaintItem>& items,
                                float viewport_width, float viewport_height) {
    if (node.style.display == UiDisplay::none) return;
    if (node.hidden_by_if) return;
    // A text node paints its glyphs here, between what comes before it in
    // the tree and what comes after.
    if (node.type == UiNodeType::text && !node.text_content.empty()) {
        items.push_back({.text_node = &node});
    }
    if (node.layout_rect.w <= 0 || node.layout_rect.h <= 0) return;

    const auto& s = node.style;
    bool has_bg = s.background_color.a > 0.0f || s.gradient.active();
    bool has_border = s.border_width > 0.0f && s.border_color.a > 0.0f;
    bool has_shadow = s.box_shadow.active();
    bool has_texture = node.resolved_texture_id != 0;
    bool has_material = node.resolved_material_id != UINT32_MAX &&
                        node.resolved_pipeline_id != UINT32_MAX;

    // Emit shadow quad first (behind the main quad)
    if (has_shadow) {
        const auto& sh = s.box_shadow;
        float expand = sh.blur + sh.spread;
        GpuUiQuad shadow{};
        shadow.rect = {node.layout_rect.x + sh.offset_x - expand,
                       node.layout_rect.y + sh.offset_y - expand,
                       node.layout_rect.w + 2.0f * expand,
                       node.layout_rect.h + 2.0f * expand};
        shadow.color = {sh.color.r, sh.color.g, sh.color.b, sh.color.a};
        shadow.corners = {s.border_radius.top_left + expand,
                          s.border_radius.top_right + expand,
                          s.border_radius.bottom_right + expand,
                          s.border_radius.bottom_left + expand};
        // flags bit 1 = shadow quad; shadow_params = blur, spread
        shadow.params = {0.0f, node.draw_opacity, 2.0f, 0.0f};
        shadow.clip_rect = {node.clip_rect.x, node.clip_rect.y,
                            node.clip_rect.w, node.clip_rect.h};
        shadow.viewport_size = {viewport_width, viewport_height};
        shadow.shadow_params = {sh.blur, sh.spread};
        shadow.texture_id = 0;
        shadow.material_id = UINT32_MAX;
        shadow.state = {1.0f, 0.0f, 0.0f, 0.0f};
        shadow.bar = {1.0f, 0.0f, 0.0f, 0.0f};
        items.push_back({.quad = static_cast<uint32_t>(quads.size())});
        quads.push_back({shadow, UINT32_MAX});
    }

    if (has_bg || has_border || has_texture || has_material) {
        // flags: 0 = normal, 1 = gradient, 2 = shadow, 3 = raw texture,
        // 4 = drawn by the material's own pipeline
        float flags = s.gradient.active() ? 1.0f : 0.0f;
        if (has_texture) flags = 3.0f;
        if (has_material) flags = 4.0f;

        float rect_w = node.layout_rect.w;

        // A bar without a material shows its fill by shrinking the quad. A
        // material paints the whole bar and reads the fill as FILL, so it
        // can shape the edge and the empty part itself. Either way the
        // displayed fill is the one the bar dynamics moved, not the raw value.
        bool is_bar = node.tag == "bar";
        if (is_bar && !has_material) {
            rect_w *= node.fill_display;
        }

        GpuUiQuad quad{};
        quad.rect = {node.layout_rect.x, node.layout_rect.y,
                     rect_w, node.layout_rect.h};

        // Default color: use background_color, or white for textured elements
        if (has_bg) {
            quad.color = {s.background_color.r, s.background_color.g,
                          s.background_color.b, s.background_color.a};
        } else {
            quad.color = {1.0f, 1.0f, 1.0f, 1.0f};
        }

        quad.border_color = {s.border_color.r, s.border_color.g,
                             s.border_color.b, s.border_color.a};
        quad.corners = {s.border_radius.top_left, s.border_radius.top_right,
                        s.border_radius.bottom_right, s.border_radius.bottom_left};
        quad.params = {s.border_width, node.draw_opacity, flags, s.gradient.angle};
        quad.clip_rect = {node.clip_rect.x, node.clip_rect.y,
                          node.clip_rect.w, node.clip_rect.h};
        if (s.gradient.active() && !has_texture && !has_material) {
            // For gradient: color = from, gradient_to = to
            quad.color = {s.gradient.from.r, s.gradient.from.g,
                          s.gradient.from.b, s.gradient.from.a};
            quad.gradient_to = {s.gradient.to.r, s.gradient.to.g,
                                s.gradient.to.b, s.gradient.to.a};
        } else if (s.gradient.active() && has_material) {
            // Material + gradient: use gradient start as base tint for the material
            quad.color = {s.gradient.from.r, s.gradient.from.g,
                          s.gradient.from.b, s.gradient.from.a};
        }
        quad.viewport_size = {viewport_width, viewport_height};
        quad.texture_id = node.resolved_texture_id;
        quad.material_id = has_material ? node.resolved_material_id : UINT32_MAX;
        quad.state = {is_bar ? node.fill_display : 1.0f,
                      node.is_hovered() ? 1.0f : 0.0f,
                      node.is_active() ? 1.0f : 0.0f,
                      node.is_focused() ? 1.0f : 0.0f};
        quad.bar = {is_bar ? node.fill_fraction : 1.0f,
                    is_bar ? node.fill_velocity : 0.0f,
                    is_bar ? node.fill_agitation : 0.0f,
                    0.0f};

        items.push_back({.quad = static_cast<uint32_t>(quads.size())});
        quads.push_back({quad, has_material ? node.resolved_pipeline_id : UINT32_MAX});
    }

    for (const auto& child : node.children) {
        collect_quads(*child, quads, items, viewport_width, viewport_height);
    }
}

bool UiRenderer::ensure_material_pipelines(uint32_t pipeline_id) {
    if (!fjsl_pipelines_) return false;

    auto it = material_pipelines_.find(pipeline_id);
    uint32_t generation = fjsl_pipelines_->generation(pipeline_id);
    if (it != material_pipelines_.end() && it->second.generation == generation) {
        return static_cast<bool>(it->second.screen);
    }

    // The entry compiled again (or for the first time). A failed compile
    // leaves the cache's SPV paths at the last good build and its generation
    // unchanged, so an existing pipeline is only replaced by a working one.
    const std::string& shader = fjsl_pipelines_->shader_path(pipeline_id);
    const std::string& screen_spv = fjsl_pipelines_->ui_frag_spv(pipeline_id);
    auto screen_fragment = screen_spv.empty() ? Result<std::vector<uint32_t>>(make_error("no ui fragment"))
                                              : gpu::read_spirv(screen_spv);
    if (!screen_fragment) {
        if (it == material_pipelines_.end()) {
            FJELL_GFX_WARN("UI material '{}' has no ui fragment to draw with", shader);
            material_pipelines_[pipeline_id].generation = generation;
        }
        return false;
    }

    MaterialPipelines fresh;
    fresh.generation = generation;
    fresh.screen =
        make(screen_desc("shaders/ui_quad.vert", std::span<const uint32_t>(*screen_fragment), shader, true));
    if (has_world_pipelines_) {
        auto world_fragment = gpu::read_spirv(fjsl_pipelines_->ui_world_frag_spv(pipeline_id));
        if (!world_fragment) {
            FJELL_GFX_ERROR("UI material '{}': {}", shader, world_fragment.error());
        } else {
            fresh.world = make(world_desc("shaders/ui_quad_world.vert",
                                          std::span<const uint32_t>(*world_fragment), shader, true));
        }
    }

    // The pipelines replaced go once the frames using them are done.
    material_pipelines_[pipeline_id] = std::move(fresh);
    FJELL_GFX_DEBUG("UI material pipelines built for '{}' (generation {})", shader, generation);
    return true;
}

void UiRenderer::record(gpu::RenderEncoder& pass, gpu::BindGroup globals, gpu::BindGroup bindless,
                        glm::uvec2 extent, uint32_t frame_index) {
    FJELL_PROFILE_SCOPE_N("ui_renderer_record");
    const auto& runs = runs_[frame_index];
    if (runs.empty()) return;

    // Glyphs clip in their shader and draw with the whole target open.
    const gpu::Rect full{.x = 0, .y = 0, .width = extent.x, .height = extent.y};

    // Clip rects are in layout pixels; scissors are in target pixels.
    const float frame_scale = frame_scales_[frame_index];
    auto scissor = [&](const glm::vec4& layout_clip) {
        const glm::vec4 clip = layout_clip * frame_scale;
        const float x = std::max(0.0f, clip.x);
        const float y = std::max(0.0f, clip.y);
        const float right = std::min(clip.x + clip.z, static_cast<float>(extent.x));
        const float bottom = std::min(clip.y + clip.w, static_cast<float>(extent.y));
        return gpu::Rect{.x = static_cast<int32_t>(x),
                         .y = static_cast<int32_t>(y),
                         .width = static_cast<uint32_t>(std::max(0.0f, right - std::floor(x))),
                         .height = static_cast<uint32_t>(std::max(0.0f, bottom - std::floor(y)))};
    };

    const gpu::BindEntry buffers[] = {
        {QUAD_BUFFER, gpu::storage(quad_buffers_[frame_index])},
        {GLYPH_BUFFER, gpu::storage(glyph_buffers_[frame_index])},
    };

    // In tree order. A run of material quads binds that material's
    // pipeline; the fixed fill and the text pipeline are rebound as the runs
    // change, and with them what they read.
    gpu::GraphicsPipeline bound{};
    for (const PaintRun& run : runs) {
        const gpu::GraphicsPipeline wanted = run.text ? text_pipeline_.get()
                                             : run.pipeline_id == UINT32_MAX
                                                 ? quad_pipeline_.get()
                                                 : material_pipelines_[run.pipeline_id].screen.get();
        if (wanted != bound) {
            pass.set_pipeline(wanted);
            if (!run.text) pass.bind(globals);
            pass.bind(bindless);
            pass.bind(buffers);
            bound = wanted;
        }
        pass.set_scissor(run.text ? full : scissor(run.clip_rect));
        pass.draw(run.count * 6, 1, run.first * 6, 0);
    }
}

void UiRenderer::create_world_pipelines(const PassResources& resources) {
    world_ = resources;
    has_world_pipelines_ = true;
    // Material pipelines built before this point know only the screen shape
    for (auto& [id, mp] : material_pipelines_) {
        mp.generation = 0;
    }

    // Drawn in the transparent pass, whose depth is read-only: world UI is
    // hidden by the scene in front of it and paints in tree order.
    world_quad_pipeline_ = make(world_desc("shaders/ui_quad_world.vert", "shaders/ui_quad_world.frag",
                                           "ui_quad_world", true));
    world_text_pipeline_ = make(world_desc("shaders/ui_text_world.vert", "shaders/ui_text_world.frag",
                                           "ui_text_world", false));

    FJELL_GFX_DEBUG("Worldspace UI pipelines created");
}

void UiRenderer::record_world(gpu::RenderEncoder& pass, gpu::BindGroup globals, gpu::BindGroup bindless,
                              glm::uvec2 extent, uint32_t frame_index, const UiWorldPushConstants& push,
                              const glm::mat4& prev_mvp) {
    FJELL_PROFILE_SCOPE_N("ui_renderer_record_world");
    if (runs_[frame_index].empty()) return;

    // Each widget's own: several are drawn in one frame.
    const gpu::BindEntry reads[] = {
        {QUAD_BUFFER, gpu::storage(quad_buffers_[frame_index])},
        {GLYPH_BUFFER, gpu::storage(glyph_buffers_[frame_index])},
        {WORLD_FRAME, gpu::uniform(pass.transient(UiWorldFrameData{prev_mvp}))},
    };

    // In tree order, as the screen UI paints; world UI does not clip.
    pass.set_scissor({.x = 0, .y = 0, .width = extent.x, .height = extent.y});
    gpu::GraphicsPipeline bound{};
    for (const PaintRun& run : runs_[frame_index]) {
        const gpu::GraphicsPipeline wanted = run.text ? world_text_pipeline_.get()
                                             : run.pipeline_id == UINT32_MAX
                                                 ? world_quad_pipeline_.get()
                                                 : material_pipelines_[run.pipeline_id].world.get();
        if (wanted != bound) {
            pass.set_pipeline(wanted);
            if (!run.text) pass.bind(globals);
            pass.bind(bindless);
            pass.bind(reads);
            pass.push(push);
            bound = wanted;
        }
        pass.draw(run.count * 6, 1, run.first * 6, 0);
    }
}

} // namespace fjell
