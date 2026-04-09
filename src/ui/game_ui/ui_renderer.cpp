#include "ui/game_ui/ui_renderer.hpp"
#include "ui/game_ui/ui_node.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/resources/shader_utils.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"

#include <algorithm>
#include <cstring>

namespace fjell {

enum class UiPipelineMode { transparent, opaque };

// Helper to create a pipeline with given shaders and shared state
static VkPipeline create_ui_pipeline(VkDevice device, VkPipelineLayout layout,
                                      VkFormat color_format, VkFormat depth_format,
                                      const std::string& vert_path,
                                      const std::string& frag_path,
                                      UiPipelineMode mode = UiPipelineMode::transparent,
                                      VkSampleCountFlagBits msaa = VK_SAMPLE_COUNT_1_BIT,
                                      VkFormat mr_format = VK_FORMAT_UNDEFINED,
                                      VkFormat normal_format = VK_FORMAT_UNDEFINED) {
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
    if (use_depth) {
        depth_stencil.depthTestEnable = VK_TRUE;
        depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        if (mode == UiPipelineMode::opaque) {
            depth_stencil.depthWriteEnable = VK_TRUE;
        } else {
            depth_stencil.depthWriteEnable = VK_FALSE;
        }
    } else {
        depth_stencil.depthTestEnable = VK_FALSE;
        depth_stencil.depthWriteEnable = VK_FALSE;
    }

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (mode == UiPipelineMode::opaque) {
        blend.blendEnable = VK_FALSE;
    } else {
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
    }

    // Second color attachment (M/R): no writes when present
    VkPipelineColorBlendAttachmentState mr_blend{};
    mr_blend.colorWriteMask = 0; // no writes to M/R from UI

    // Third color attachment (normal): no writes from UI
    VkPipelineColorBlendAttachmentState normal_blend{};
    normal_blend.colorWriteMask = 0;

    bool has_mr = (mr_format != VK_FORMAT_UNDEFINED);
    bool has_normal = (normal_format != VK_FORMAT_UNDEFINED);
    uint32_t attachment_count = 1 + (has_mr ? 1 : 0) + (has_normal ? 1 : 0);
    VkPipelineColorBlendAttachmentState blend_attachments[] = {blend, mr_blend, normal_blend};

    VkPipelineColorBlendStateCreateInfo color_blending{};
    color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blending.attachmentCount = attachment_count;
    color_blending.pAttachments = blend_attachments;

    VkFormat color_formats[] = {color_format, mr_format, normal_format};

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
                        VkFormat output_format) {
    device_ = device.handle();
    allocator_ = allocator;
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

    // Transparent quad pipeline (blend ON, depth test ON, depth write OFF)
    pipeline_ = create_ui_pipeline(device_, layout_, color_format_, depth_format_,
        base_path::resolve("shaders/ui_quad.vert.spv"),
        base_path::resolve("shaders/ui_quad.frag.spv"),
        UiPipelineMode::transparent);

    // Opaque quad pipeline (blend OFF, depth test ON, depth write ON)
    opaque_pipeline_ = create_ui_pipeline(device_, layout_, color_format_, depth_format_,
        base_path::resolve("shaders/ui_quad.vert.spv"),
        base_path::resolve("shaders/ui_quad.frag.spv"),
        UiPipelineMode::opaque);

    // Text pipeline (always transparent)
    text_pipeline_ = create_ui_pipeline(device_, layout_, color_format_, depth_format_,
        base_path::resolve("shaders/ui_text.vert.spv"),
        base_path::resolve("shaders/ui_text.frag.spv"),
        UiPipelineMode::transparent);

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
    safe_destroy(pool_, vkDestroyDescriptorPool);
    safe_destroy(world_text_pipeline_, vkDestroyPipeline);
    safe_destroy(world_transparent_pipeline_, vkDestroyPipeline);
    safe_destroy(world_opaque_pipeline_, vkDestroyPipeline);
    safe_destroy(world_layout_, vkDestroyPipelineLayout);
    safe_destroy(text_pipeline_, vkDestroyPipeline);
    safe_destroy(opaque_pipeline_, vkDestroyPipeline);
    safe_destroy(pipeline_, vkDestroyPipeline);
    safe_destroy(layout_, vkDestroyPipelineLayout);
    safe_destroy(quad_set_layout_, vkDestroyDescriptorSetLayout);
}

std::vector<UiRenderer::ClipGroup> UiRenderer::build_clip_groups(
        const GpuUiQuad* quads, uint32_t first_quad, uint32_t quad_count) {
    std::vector<ClipGroup> groups;
    if (quad_count == 0) return groups;

    ClipGroup current{};
    current.clip_rect = quads[first_quad].clip_rect;
    current.first_vertex = first_quad * 6;
    current.vertex_count = 6;

    for (uint32_t i = 1; i < quad_count; ++i) {
        const auto& cr = quads[first_quad + i].clip_rect;
        if (cr == current.clip_rect) {
            current.vertex_count += 6;
        } else {
            groups.push_back(current);
            current.clip_rect = cr;
            current.first_vertex = (first_quad + i) * 6;
            current.vertex_count = 6;
        }
    }
    groups.push_back(current);
    return groups;
}

void UiRenderer::update(const UiNode& root, float viewport_width, float viewport_height,
                         uint32_t frame_index) {
    FJELL_PROFILE_SCOPE_N("ui_renderer_update");
    std::vector<GpuUiQuad> quads;
    collect_quads(root, quads, viewport_width, viewport_height);

    // Partition into opaque (front-to-back) and transparent (original order)
    std::vector<GpuUiQuad> opaque;
    std::vector<GpuUiQuad> transparent;
    for (auto& q : quads) {
        if (q.quad_flags & 1u) {
            opaque.push_back(q);
        } else {
            transparent.push_back(q);
        }
    }

    // Sort opaque front-to-back (ascending z_layer = near first)
    std::sort(opaque.begin(), opaque.end(),
              [](const GpuUiQuad& a, const GpuUiQuad& b) {
                  return a.z_layer < b.z_layer;
              });

    // Write opaque first, then transparent
    std::vector<GpuUiQuad> sorted;
    sorted.reserve(opaque.size() + transparent.size());
    sorted.insert(sorted.end(), opaque.begin(), opaque.end());
    sorted.insert(sorted.end(), transparent.begin(), transparent.end());

    uint32_t count = std::min(static_cast<uint32_t>(sorted.size()), MAX_UI_QUADS);
    quad_counts_[frame_index] = count;
    uint32_t opaque_count = std::min(static_cast<uint32_t>(opaque.size()), count);
    opaque_counts_[frame_index] = opaque_count;
    uint32_t transparent_count = count - opaque_count;

    if (count > 0) {
        auto* dst = static_cast<GpuUiQuad*>(quad_ssbos_[frame_index]->mapped());
        std::memcpy(dst, sorted.data(), count * sizeof(GpuUiQuad));
    }

    // Build scissor clip groups for each pass
    opaque_groups_[frame_index] = build_clip_groups(sorted.data(), 0, opaque_count);
    transparent_groups_[frame_index] = build_clip_groups(sorted.data(), opaque_count, transparent_count);

    // Reset text count (will be filled by update_text calls)
    text_counts_[frame_index] = 0;
}

void UiRenderer::update_text(const std::vector<PositionedGlyph>& glyphs,
                              const UiNode& source_node,
                              uint32_t atlas_index, float pixel_range,
                              float viewport_width, float viewport_height,
                              uint32_t frame_index) {
    if (glyphs.empty()) return;

    uint32_t existing = text_counts_[frame_index];
    uint32_t count = std::min(static_cast<uint32_t>(glyphs.size()),
                              MAX_TEXT_GLYPHS - existing);
    if (count == 0) return;

    auto* dst = static_cast<GpuUiGlyph*>(text_ssbos_[frame_index]->mapped()) + existing;

    const auto& tc = source_node.style.color;

    for (uint32_t i = 0; i < count; ++i) {
        const auto& pg = glyphs[i];
        GpuUiGlyph gg{};
        gg.rect = {source_node.layout_rect.x + pg.x,
                   source_node.layout_rect.y + pg.y,
                   pg.w, pg.h};
        gg.uv_rect = {pg.uv_x0, pg.uv_y0, pg.uv_x1, pg.uv_y1};
        gg.color = {tc.r, tc.g, tc.b, tc.a};
        gg.params = {source_node.style.opacity,
                     static_cast<float>(atlas_index), pixel_range, 0.0f};
        gg.clip_rect = {source_node.clip_rect.x, source_node.clip_rect.y,
                        source_node.clip_rect.w, source_node.clip_rect.h};
        gg.viewport_size = {viewport_width, viewport_height};
        dst[i] = gg;
    }

    text_counts_[frame_index] = existing + count;
}

void UiRenderer::collect_quads(const UiNode& node, std::vector<GpuUiQuad>& quads,
                                float viewport_width, float viewport_height) {
    if (node.style.display == UiDisplay::none) return;
    if (node.hidden_by_if) return;
    if (node.layout_rect.w <= 0 || node.layout_rect.h <= 0) return;

    const auto& s = node.style;
    bool has_bg = s.background_color.a > 0.0f || s.gradient.active();
    bool has_border = s.border_width > 0.0f && s.border_color.a > 0.0f;
    bool has_shadow = s.box_shadow.active();
    bool has_texture = node.resolved_texture_id != 0;
    bool has_material = node.resolved_material_id != UINT32_MAX;

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
        shadow.params = {0.0f, s.opacity, 2.0f, 0.0f};
        shadow.clip_rect = {node.clip_rect.x, node.clip_rect.y,
                            node.clip_rect.w, node.clip_rect.h};
        shadow.viewport_size = {viewport_width, viewport_height};
        shadow.shadow_params = {sh.blur, sh.spread};
        shadow.texture_id = 0;
        shadow.material_id = UINT32_MAX;
        shadow.z_layer = 1.0f - static_cast<float>(quads.size()) / MAX_UI_QUADS;
        shadow.quad_flags = 0;
        quads.push_back(shadow);
    }

    if (has_bg || has_border || has_texture || has_material) {
        // flags: 0 = normal, 1 = gradient, 2 = shadow, 3 = raw texture, 4 = material
        float flags = s.gradient.active() ? 1.0f : 0.0f;
        if (has_texture) flags = 3.0f;
        if (has_material) flags = 4.0f;

        float rect_w = node.layout_rect.w;

        // Bar element: shrink background quad to fill percentage
        if (node.tag == "bar") {
            auto pct_it = node.attributes.find("_fill_pct");
            if (pct_it != node.attributes.end()) {
                float pct = 0.0f;
                try { pct = std::stof(pct_it->second); } catch (...) {}
                rect_w *= pct;
            }
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
        quad.params = {s.border_width, s.opacity, flags, s.gradient.angle};
        quad.clip_rect = {node.clip_rect.x, node.clip_rect.y,
                          node.clip_rect.w, node.clip_rect.h};
        if (s.gradient.active() && !has_texture && !has_material) {
            // For gradient: color = from, gradient_to = to
            quad.color = {s.gradient.from.r, s.gradient.from.g,
                          s.gradient.from.b, s.gradient.from.a};
            quad.gradient_to = {s.gradient.to.r, s.gradient.to.g,
                                s.gradient.to.b, s.gradient.to.a};
        }
        quad.viewport_size = {viewport_width, viewport_height};
        quad.texture_id = node.resolved_texture_id;
        quad.material_id = node.resolved_material_id;
        quad.z_layer = 1.0f - static_cast<float>(quads.size()) / MAX_UI_QUADS;

        // Classify opaque: fully opaque color, no border-radius, not shadow, not textured
        bool is_opaque = s.opacity >= 1.0f &&
                         quad.color.a >= 1.0f &&
                         s.border_radius.top_left == 0.0f &&
                         s.border_radius.top_right == 0.0f &&
                         s.border_radius.bottom_right == 0.0f &&
                         s.border_radius.bottom_left == 0.0f &&
                         !has_texture && !has_material;
        quad.quad_flags = is_opaque ? 1u : 0u;

        quads.push_back(quad);
    }

    for (const auto& child : node.children) {
        collect_quads(*child, quads, viewport_width, viewport_height);
    }
}

void UiRenderer::record(VkCommandBuffer cmd, VkDescriptorSet global_set, VkDescriptorSet bindless_set,
                         VkExtent2D extent, uint32_t frame_index) {
    FJELL_PROFILE_SCOPE_N("ui_renderer_record");
    uint32_t total_quads = quad_counts_[frame_index];
    bool has_text = text_counts_[frame_index] > 0;
    if (total_quads == 0 && !has_text) return;

    VkViewport vp{};
    vp.width = static_cast<float>(extent.width);
    vp.height = static_cast<float>(extent.height);
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    // Full-extent scissor (default, restored between groups)
    VkRect2D full_scissor{};
    full_scissor.extent = extent;

    // Bind shared descriptor sets
    VkDescriptorSet sets[] = {global_set, bindless_set, quad_sets_[frame_index]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_,
                            0, 3, sets, 0, nullptr);

    // Helper: set scissor from a clip_rect (pixels), clamped to extent
    auto set_scissor = [&](const glm::vec4& clip) {
        VkRect2D sc{};
        sc.offset.x = static_cast<int32_t>(std::max(0.0f, clip.x));
        sc.offset.y = static_cast<int32_t>(std::max(0.0f, clip.y));
        float right = std::min(clip.x + clip.z, static_cast<float>(extent.width));
        float bottom = std::min(clip.y + clip.w, static_cast<float>(extent.height));
        sc.extent.width = static_cast<uint32_t>(std::max(0.0f, right - static_cast<float>(sc.offset.x)));
        sc.extent.height = static_cast<uint32_t>(std::max(0.0f, bottom - static_cast<float>(sc.offset.y)));
        vkCmdSetScissor(cmd, 0, 1, &sc);
    };

    // Pass 1: Draw opaque quads with per-group scissor
    const auto& opaque_groups = opaque_groups_[frame_index];
    if (!opaque_groups.empty()) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, opaque_pipeline_);
        for (const auto& group : opaque_groups) {
            set_scissor(group.clip_rect);
            vkCmdDraw(cmd, group.vertex_count, 1, group.first_vertex, 0);
        }
    }

    // Pass 2: Draw transparent quads with per-group scissor
    const auto& transparent_groups = transparent_groups_[frame_index];
    if (!transparent_groups.empty()) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        for (const auto& group : transparent_groups) {
            set_scissor(group.clip_rect);
            vkCmdDraw(cmd, group.vertex_count, 1, group.first_vertex, 0);
        }
    }

    // Draw text glyphs (full scissor, text has its own clip test)
    if (has_text) {
        vkCmdSetScissor(cmd, 0, 1, &full_scissor);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, text_pipeline_);
        vkCmdDraw(cmd, text_counts_[frame_index] * 6, 1, 0, 0);
    }
}

void UiRenderer::create_world_pipelines(Device& /*device*/, const PassResources& resources) {
    // Pipeline layout: set 0 = global, set 1 = bindless, set 2 = UI SSBOs + push constants
    VkDescriptorSetLayout sets[] = {global_set_layout_, bindless_set_layout_, quad_set_layout_};

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

    // Color pass has 2 color attachments (color + metallic/roughness).
    // UI doesn't write to M/R, but the pipeline must declare both.
    constexpr VkFormat mr_fmt = VK_FORMAT_R8G8_UNORM;

    auto normal_fmt = OffscreenPass::NORMAL_FORMAT;

    world_transparent_pipeline_ = create_ui_pipeline(device_, world_layout_, color_fmt, depth_fmt,
        base_path::resolve("shaders/ui_quad_world.vert.spv"),
        base_path::resolve("shaders/ui_quad.frag.spv"),
        UiPipelineMode::transparent, msaa, mr_fmt, normal_fmt);

    world_opaque_pipeline_ = create_ui_pipeline(device_, world_layout_, color_fmt, depth_fmt,
        base_path::resolve("shaders/ui_quad_world.vert.spv"),
        base_path::resolve("shaders/ui_quad.frag.spv"),
        UiPipelineMode::opaque, msaa, mr_fmt, normal_fmt);

    world_text_pipeline_ = create_ui_pipeline(device_, world_layout_, color_fmt, depth_fmt,
        base_path::resolve("shaders/ui_text_world.vert.spv"),
        base_path::resolve("shaders/ui_text.frag.spv"),
        UiPipelineMode::transparent, msaa, mr_fmt, normal_fmt);

    FJELL_GFX_DEBUG("Worldspace UI pipelines created ({}x MSAA)", static_cast<uint32_t>(msaa));
}

void UiRenderer::record_world(VkCommandBuffer cmd, VkDescriptorSet global_set, VkDescriptorSet bindless_set,
                               VkExtent2D extent, uint32_t frame_index, const UiWorldPushConstants& push) {
    FJELL_PROFILE_SCOPE_N("ui_renderer_record_world");
    uint32_t total_quads = quad_counts_[frame_index];
    bool has_text = text_counts_[frame_index] > 0;
    if (total_quads == 0 && !has_text) return;

    VkViewport vp{};
    vp.width = static_cast<float>(extent.width);
    vp.height = static_cast<float>(extent.height);
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D full_scissor{};
    full_scissor.extent = extent;
    vkCmdSetScissor(cmd, 0, 1, &full_scissor);

    // Bind shared descriptor sets on worldspace layout
    VkDescriptorSet desc_sets[] = {global_set, bindless_set, quad_sets_[frame_index]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_layout_,
                            0, 3, desc_sets, 0, nullptr);

    // Push worldspace constants
    vkCmdPushConstants(cmd, world_layout_, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(UiWorldPushConstants), &push);

    // Pass 1: Opaque quads
    const auto& opaque_groups = opaque_groups_[frame_index];
    if (!opaque_groups.empty()) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_opaque_pipeline_);
        for (const auto& group : opaque_groups) {
            vkCmdDraw(cmd, group.vertex_count, 1, group.first_vertex, 0);
        }
    }

    // Pass 2: Transparent quads
    const auto& transparent_groups = transparent_groups_[frame_index];
    if (!transparent_groups.empty()) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_transparent_pipeline_);
        for (const auto& group : transparent_groups) {
            vkCmdDraw(cmd, group.vertex_count, 1, group.first_vertex, 0);
        }
    }

    // Pass 3: Text glyphs
    if (has_text) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_text_pipeline_);
        vkCmdDraw(cmd, text_counts_[frame_index] * 6, 1, 0, 0);
    }
}

} // namespace fjell
