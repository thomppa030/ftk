#include "ui/game_ui/ui_renderer.hpp"
#include "ui/game_ui/ui_node.hpp"
#include "gpu/vulkan/native.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/gpu/shader_utils.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"

#include <algorithm>
#include <cstring>

namespace fjell {

// A UI pipeline: quads or glyphs blended over what is there, in the order
// they are drawn. With a depth format it tests against that depth (the
// scene, for world UI) and never writes it.
static VkPipeline create_ui_pipeline(VkDevice device, VkPipelineLayout layout,
                                      VkFormat color_format, VkFormat depth_format,
                                      const std::string& vert_path,
                                      const std::string& frag_path,
                                      VkSampleCountFlagBits msaa = VK_SAMPLE_COUNT_1_BIT,
                                      VkFormat mr_format = VK_FORMAT_UNDEFINED,
                                      VkFormat normal_format = VK_FORMAT_UNDEFINED,
                                      VkFormat velocity_format = VK_FORMAT_UNDEFINED) {
    auto vert_mod = create_shader_module(device, read_spirv(vert_path));
    auto frag_mod = create_shader_module(device, read_spirv(frag_path));

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert_mod;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag_mod;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = 2;
    dynamic_state.pDynamicStates = dynamic_states;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = msaa;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    bool use_depth = (depth_format != VK_FORMAT_UNDEFINED);
    depth_stencil.depthTestEnable = use_depth ? VK_TRUE : VK_FALSE;
    depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    depth_stencil.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;

    // Second color attachment (M/R): no writes when present
    VkPipelineColorBlendAttachmentState mr_blend{};
    mr_blend.colorWriteMask = 0; // no writes to M/R from UI

    // Third color attachment (normal): no writes from UI
    VkPipelineColorBlendAttachmentState normal_blend{};
    normal_blend.colorWriteMask = 0;

    // Fourth color attachment (velocity): only ever declared for worldspace
    // pipelines (velocity_format is VK_FORMAT_UNDEFINED for screen-space UI,
    // which excludes this attachment from the pipeline entirely — see
    // has_velocity below). The worldspace fragment shaders compute a real
    // per-pixel velocity now, so this writes unlike the M/R and normal
    // attachments above, which the UI genuinely has no data for.
    VkPipelineColorBlendAttachmentState vel_blend{};
    vel_blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;

    bool has_mr = (mr_format != VK_FORMAT_UNDEFINED);
    bool has_normal = (normal_format != VK_FORMAT_UNDEFINED);
    bool has_velocity = (velocity_format != VK_FORMAT_UNDEFINED);
    uint32_t attachment_count = 1 + (has_mr ? 1 : 0) + (has_normal ? 1 : 0) + (has_velocity ? 1 : 0);
    VkPipelineColorBlendAttachmentState blend_attachments[] = {blend, mr_blend, normal_blend, vel_blend};

    VkPipelineColorBlendStateCreateInfo color_blending{};
    color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blending.attachmentCount = attachment_count;
    color_blending.pAttachments = blend_attachments;

    VkFormat color_formats[] = {color_format, mr_format, normal_format, velocity_format};

    VkPipelineRenderingCreateInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_info.colorAttachmentCount = attachment_count;
    rendering_info.pColorAttachmentFormats = color_formats;
    if (use_depth) {
        rendering_info.depthAttachmentFormat = depth_format;
    }

    VkGraphicsPipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.pNext = &rendering_info;
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vertex_input;
    ci.pInputAssemblyState = &input_assembly;
    ci.pViewportState = &viewport_state;
    ci.pRasterizationState = &rasterizer;
    ci.pMultisampleState = &multisampling;
    ci.pDepthStencilState = &depth_stencil;
    ci.pColorBlendState = &color_blending;
    ci.pDynamicState = &dynamic_state;
    ci.layout = layout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create UI pipeline");
    }

    vkDestroyShaderModule(device, vert_mod, nullptr);
    vkDestroyShaderModule(device, frag_mod, nullptr);
    return pipeline;
}

void UiRenderer::create(Device& device, VmaAllocator allocator,
                        const PassResources& resources,
                        PipelineCache* fjsl_pipelines, gpu::Device& gpu,
                        VkFormat output_format) {
    device_ = device.handle();
    allocator_ = allocator;
    fjsl_pipelines_ = fjsl_pipelines;
    gpu_device_ = &gpu;
    global_set_layout_ = resources.global_set_layout;
    bindless_set_layout_ = resources.material_set_layout;
    color_format_ = (output_format != VK_FORMAT_UNDEFINED) ? output_format : resources.color_format;
    depth_format_ = resources.depth_format;

    // Set 2: binding 0 = quad SSBO, binding 1 = glyph SSBO
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_ci.bindingCount = 2;
    layout_ci.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device_, &layout_ci, nullptr, &quad_set_layout_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create UI set layout");
    }

    // Pipeline layout: set 0 = global, set 1 = bindless, set 2 = UI SSBOs
    VkDescriptorSetLayout sets[] = {global_set_layout_, bindless_set_layout_, quad_set_layout_};
    VkPipelineLayoutCreateInfo pipe_layout_ci{};
    pipe_layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipe_layout_ci.setLayoutCount = 3;
    pipe_layout_ci.pSetLayouts = sets;
    if (vkCreatePipelineLayout(device_, &pipe_layout_ci, nullptr, &layout_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create UI pipeline layout");
    }

    // Screen UI draws into a scope with no depth: it paints in tree order.
    pipeline_ = create_ui_pipeline(device_, layout_, color_format_, VK_FORMAT_UNDEFINED,
        base_path::resolve("shaders/ui_quad.vert.spv"),
        base_path::resolve("shaders/ui_quad.frag.spv"));
    text_pipeline_ = create_ui_pipeline(device_, layout_, color_format_, VK_FORMAT_UNDEFINED,
        base_path::resolve("shaders/ui_text.vert.spv"),
        base_path::resolve("shaders/ui_text.frag.spv"));

    // N sets × 2 SSBOs each = 2N storage buffer descriptors
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = MAX_FRAMES_IN_FLIGHT * 2;

    VkDescriptorPoolCreateInfo desc_pool_ci{};
    desc_pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    desc_pool_ci.maxSets = MAX_FRAMES_IN_FLIGHT;
    desc_pool_ci.poolSizeCount = 1;
    desc_pool_ci.pPoolSizes = &pool_size;
    if (vkCreateDescriptorPool(device_, &desc_pool_ci, nullptr, &pool_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create UI descriptor pool");
    }

    // Double-buffered SSBOs + descriptor sets
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        quad_ssbos_[i] = std::make_unique<Buffer>(
            allocator_, SSBO_SIZE,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
        quad_ssbos_[i]->map();

        text_ssbos_[i] = std::make_unique<Buffer>(
            allocator_, TEXT_SSBO_SIZE,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
        text_ssbos_[i]->map();

        VkDescriptorSetAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool = pool_;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &quad_set_layout_;
        if (vkAllocateDescriptorSets(device_, &alloc, &quad_sets_[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to allocate UI descriptor set");
        }

        // Write both bindings
        VkDescriptorBufferInfo quad_buf{quad_ssbos_[i]->handle(), 0, SSBO_SIZE};
        VkDescriptorBufferInfo text_buf{text_ssbos_[i]->handle(), 0, TEXT_SSBO_SIZE};

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = quad_sets_[i];
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo = &quad_buf;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = quad_sets_[i];
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].descriptorCount = 1;
        writes[1].pBufferInfo = &text_buf;

        vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
    }

    FJELL_GFX_DEBUG("UI renderer created (quad + text pipelines)");
}

void UiRenderer::destroy() {
    auto safe_destroy = [&](auto& handle, auto destroy_fn) {
        if (handle != VK_NULL_HANDLE) { destroy_fn(device_, handle, nullptr); handle = VK_NULL_HANDLE; }
    };

    for (auto& ssbo : quad_ssbos_) ssbo.reset();
    for (auto& ssbo : text_ssbos_) ssbo.reset();
    for (auto& buf : world_frame_buffers_) buf.reset();
    for (auto& [id, mp] : material_pipelines_) {
        safe_destroy(mp.screen, vkDestroyPipeline);
        safe_destroy(mp.world, vkDestroyPipeline);
    }
    material_pipelines_.clear();
    safe_destroy(pool_, vkDestroyDescriptorPool);
    safe_destroy(world_pool_, vkDestroyDescriptorPool);
    safe_destroy(world_text_pipeline_, vkDestroyPipeline);
    safe_destroy(world_transparent_pipeline_, vkDestroyPipeline);
    safe_destroy(world_layout_, vkDestroyPipelineLayout);
    safe_destroy(world_set_layout_, vkDestroyDescriptorSetLayout);
    safe_destroy(text_pipeline_, vkDestroyPipeline);
    safe_destroy(pipeline_, vkDestroyPipeline);
    safe_destroy(layout_, vkDestroyPipelineLayout);
    safe_destroy(quad_set_layout_, vkDestroyDescriptorSetLayout);
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
        auto* dst = static_cast<GpuUiQuad*>(quad_ssbos_[frame_index]->mapped());
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

    auto* base = static_cast<GpuUiGlyph*>(text_ssbos_[frame_index]->mapped());
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
            gg.params = {source_node.draw_opacity, static_cast<float>(atlas_index), pixel_range, blur};
            gg.clip_rect = {source_node.clip_rect.x, source_node.clip_rect.y,
                            source_node.clip_rect.w, source_node.clip_rect.h};
            gg.viewport_size = {viewport_width, viewport_height};
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
        return it->second.screen != VK_NULL_HANDLE;
    }

    // The entry compiled again (or for the first time). A failed compile
    // leaves the cache's SPV paths at the last good build and its generation
    // unchanged, so an existing pipeline is only replaced by a working one.
    const std::string& screen_spv = fjsl_pipelines_->ui_frag_spv(pipeline_id);
    if (screen_spv.empty()) {
        if (it == material_pipelines_.end()) {
            FJELL_GFX_WARN("UI material '{}' has no ui fragment to draw with",
                           fjsl_pipelines_->shader_path(pipeline_id));
            material_pipelines_[pipeline_id] = MaterialPipelines{VK_NULL_HANDLE, VK_NULL_HANDLE, generation};
        }
        return false;
    }

    MaterialPipelines fresh{};
    fresh.generation = generation;
    fresh.screen = create_ui_pipeline(device_, layout_, color_format_, VK_FORMAT_UNDEFINED,
        base_path::resolve("shaders/ui_quad.vert.spv"), screen_spv);
    if (has_world_pipelines_) {
        fresh.world = create_ui_pipeline(device_, world_layout_, world_color_format_, depth_format_,
            base_path::resolve("shaders/ui_quad_world.vert.spv"),
            fjsl_pipelines_->ui_world_frag_spv(pipeline_id),
            world_samples_, VK_FORMAT_R8G8_UNORM,
            OffscreenPass::NORMAL_FORMAT, OffscreenPass::VELOCITY_FORMAT);
    }

    if (it != material_pipelines_.end()) {
        retire_material_pipelines(it->second);
        it->second = fresh;
    } else {
        material_pipelines_[pipeline_id] = fresh;
    }
    FJELL_GFX_DEBUG("UI material pipelines built for '{}' (generation {})",
                    fjsl_pipelines_->shader_path(pipeline_id), generation);
    return true;
}

void UiRenderer::retire_material_pipelines(MaterialPipelines& mp) {
    // A frame in flight may still reference these; they are destroyed once
    // the GPU has finished it.
    for (VkPipeline* p : {&mp.screen, &mp.world}) {
        if (*p == VK_NULL_HANDLE) continue;
        gpu::vulkan::defer(*gpu_device_, [device = device_, pipeline = *p] {
            vkDestroyPipeline(device, pipeline, nullptr);
        });
        *p = VK_NULL_HANDLE;
    }
}

void UiRenderer::record(VkCommandBuffer cmd, VkDescriptorSet global_set, VkDescriptorSet bindless_set,
                         VkExtent2D extent, uint32_t frame_index) {
    FJELL_PROFILE_SCOPE_N("ui_renderer_record");
    const auto& runs = runs_[frame_index];
    if (runs.empty()) return;

    VkViewport vp{};
    vp.width = static_cast<float>(extent.width);
    vp.height = static_cast<float>(extent.height);
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    // Glyphs clip in their shader and draw with the whole target open.
    VkRect2D full_scissor{};
    full_scissor.extent = extent;

    // Bind shared descriptor sets
    VkDescriptorSet sets[] = {global_set, bindless_set, quad_sets_[frame_index]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_,
                            0, 3, sets, 0, nullptr);

    // Clip rects are in layout pixels; scissors are in target pixels.
    float frame_scale = frame_scales_[frame_index];

    // Helper: set scissor from a clip_rect (layout pixels), clamped to extent
    auto set_scissor = [&](const glm::vec4& layout_clip) {
        glm::vec4 clip = layout_clip * frame_scale;
        VkRect2D sc{};
        sc.offset.x = static_cast<int32_t>(std::max(0.0f, clip.x));
        sc.offset.y = static_cast<int32_t>(std::max(0.0f, clip.y));
        float right = std::min(clip.x + clip.z, static_cast<float>(extent.width));
        float bottom = std::min(clip.y + clip.w, static_cast<float>(extent.height));
        sc.extent.width = static_cast<uint32_t>(std::max(0.0f, right - static_cast<float>(sc.offset.x)));
        sc.extent.height = static_cast<uint32_t>(std::max(0.0f, bottom - static_cast<float>(sc.offset.y)));
        vkCmdSetScissor(cmd, 0, 1, &sc);
    };

    // In tree order. A run of material quads binds that material's
    // pipeline; the fixed fill and the text pipeline are rebound as the runs
    // change.
    VkPipeline bound = VK_NULL_HANDLE;
    for (const PaintRun& run : runs) {
        const VkPipeline wanted = run.text                        ? text_pipeline_
                                  : run.pipeline_id == UINT32_MAX ? pipeline_
                                                                  : material_pipelines_[run.pipeline_id].screen;
        if (wanted != bound) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, wanted);
            bound = wanted;
        }
        if (run.text) {
            vkCmdSetScissor(cmd, 0, 1, &full_scissor);
        } else {
            set_scissor(run.clip_rect);
        }
        vkCmdDraw(cmd, run.count * 6, 1, run.first * 6, 0);
    }
}

void UiRenderer::create_world_pipelines(Device& /*device*/, const PassResources& resources) {
    // Worldspace gets its own set 2 layout instead of sharing quad_set_layout_:
    // it needs a third binding (the previous-frame mvp UBO, for the velocity
    // attachment) that the screen-space pipelines have no use for and would
    // otherwise be forced to declare unused.
    VkDescriptorSetLayoutBinding bindings[3]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo world_layout_ci{};
    world_layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    world_layout_ci.bindingCount = 3;
    world_layout_ci.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device_, &world_layout_ci, nullptr, &world_set_layout_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create worldspace UI set layout");
    }

    // Pipeline layout: set 0 = global, set 1 = bindless, set 2 = worldspace UI set + push constants
    VkDescriptorSetLayout sets[] = {global_set_layout_, bindless_set_layout_, world_set_layout_};

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(UiWorldPushConstants);

    VkPipelineLayoutCreateInfo pipe_layout_ci{};
    pipe_layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipe_layout_ci.setLayoutCount = 3;
    pipe_layout_ci.pSetLayouts = sets;
    pipe_layout_ci.pushConstantRangeCount = 1;
    pipe_layout_ci.pPushConstantRanges = &push_range;
    if (vkCreatePipelineLayout(device_, &pipe_layout_ci, nullptr, &world_layout_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create worldspace UI pipeline layout");
    }

    auto msaa = resources.sample_count;
    auto color_fmt = resources.color_format;
    auto depth_fmt = resources.depth_format;
    has_world_pipelines_ = true;
    world_samples_ = msaa;
    world_color_format_ = color_fmt;
    // Material pipelines built before this point know only the screen shape
    for (auto& [id, mp] : material_pipelines_) {
        mp.generation = 0;
    }

    // Color pass has 2 color attachments (color + metallic/roughness).
    // UI doesn't write to M/R, but the pipeline must declare both.
    constexpr VkFormat mr_fmt = VK_FORMAT_R8G8_UNORM;

    auto normal_fmt = OffscreenPass::NORMAL_FORMAT;
    auto vel_fmt = OffscreenPass::VELOCITY_FORMAT;

    // Drawn in the transparent pass, whose depth is read-only: world UI is
    // hidden by the scene in front of it and paints in tree order.
    world_transparent_pipeline_ = create_ui_pipeline(device_, world_layout_, color_fmt, depth_fmt,
        base_path::resolve("shaders/ui_quad_world.vert.spv"),
        base_path::resolve("shaders/ui_quad_world.frag.spv"),
        msaa, mr_fmt, normal_fmt, vel_fmt);

    world_text_pipeline_ = create_ui_pipeline(device_, world_layout_, color_fmt, depth_fmt,
        base_path::resolve("shaders/ui_text_world.vert.spv"),
        base_path::resolve("shaders/ui_text_world.frag.spv"),
        msaa, mr_fmt, normal_fmt, vel_fmt);

    // Descriptor pool for the worldspace set: 2 SSBOs (quads, glyphs, shared
    // with the screen-space buffers) + 1 UBO (prev_mvp) per frame in flight.
    VkDescriptorPoolSize pool_sizes[2]{};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_sizes[0].descriptorCount = MAX_FRAMES_IN_FLIGHT * 2;
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pool_sizes[1].descriptorCount = MAX_FRAMES_IN_FLIGHT;

    VkDescriptorPoolCreateInfo world_pool_ci{};
    world_pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    world_pool_ci.poolSizeCount = 2;
    world_pool_ci.pPoolSizes = pool_sizes;
    world_pool_ci.maxSets = MAX_FRAMES_IN_FLIGHT;
    if (vkCreateDescriptorPool(device_, &world_pool_ci, nullptr, &world_pool_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create worldspace UI descriptor pool");
    }

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        world_frame_buffers_[i] = std::make_unique<Buffer>(allocator_, sizeof(UiWorldFrameData),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
        world_frame_buffers_[i]->map();

        VkDescriptorSetAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool = world_pool_;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &world_set_layout_;
        if (vkAllocateDescriptorSets(device_, &alloc, &world_sets_[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to allocate worldspace UI descriptor set");
        }

        VkDescriptorBufferInfo quad_buf{quad_ssbos_[i]->handle(), 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo text_buf{text_ssbos_[i]->handle(), 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo frame_buf{world_frame_buffers_[i]->handle(), 0, sizeof(UiWorldFrameData)};

        VkWriteDescriptorSet writes[3]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = world_sets_[i];
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo = &quad_buf;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = world_sets_[i];
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].descriptorCount = 1;
        writes[1].pBufferInfo = &text_buf;

        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = world_sets_[i];
        writes[2].dstBinding = 2;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[2].descriptorCount = 1;
        writes[2].pBufferInfo = &frame_buf;

        vkUpdateDescriptorSets(device_, 3, writes, 0, nullptr);
    }

    FJELL_GFX_DEBUG("Worldspace UI pipelines created ({}x MSAA)", static_cast<uint32_t>(msaa));
}

void UiRenderer::record_world(VkCommandBuffer cmd, VkDescriptorSet global_set, VkDescriptorSet bindless_set,
                               VkExtent2D extent, uint32_t frame_index, const UiWorldPushConstants& push,
                               const glm::mat4& prev_mvp) {
    FJELL_PROFILE_SCOPE_N("ui_renderer_record_world");
    if (runs_[frame_index].empty()) return;

    UiWorldFrameData frame_data{prev_mvp};
    std::memcpy(world_frame_buffers_[frame_index]->mapped(), &frame_data, sizeof(frame_data));

    VkViewport vp{};
    vp.width = static_cast<float>(extent.width);
    vp.height = static_cast<float>(extent.height);
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D full_scissor{};
    full_scissor.extent = extent;
    vkCmdSetScissor(cmd, 0, 1, &full_scissor);

    // Bind descriptor sets on worldspace layout — its own set 2, not
    // quad_sets_ (that one has no prev_mvp binding for the screen-space
    // pipelines that share it).
    VkDescriptorSet desc_sets[] = {global_set, bindless_set, world_sets_[frame_index]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_layout_,
                            0, 3, desc_sets, 0, nullptr);

    // Push worldspace constants
    vkCmdPushConstants(cmd, world_layout_, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(UiWorldPushConstants), &push);

    // In tree order, as the screen UI paints; world UI does not clip.
    VkPipeline bound = VK_NULL_HANDLE;
    for (const PaintRun& run : runs_[frame_index]) {
        const VkPipeline wanted = run.text                        ? world_text_pipeline_
                                  : run.pipeline_id == UINT32_MAX ? world_transparent_pipeline_
                                                                  : material_pipelines_[run.pipeline_id].world;
        if (wanted != bound) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, wanted);
            bound = wanted;
        }
        vkCmdDraw(cmd, run.count * 6, 1, run.first * 6, 0);
    }
}

} // namespace fjell
