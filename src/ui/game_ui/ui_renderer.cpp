#include "ui/game_ui/ui_renderer.hpp"
#include "ui/game_ui/ui_node.hpp"
#include "renderer/device.hpp"
#include "renderer/shader_utils.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <cstring>

namespace fjell {

// Helper to create a pipeline with given shaders and shared state
static VkPipeline create_ui_pipeline(VkDevice device, VkPipelineLayout layout,
                                      VkFormat color_format,
                                      const std::string& vert_path,
                                      const std::string& frag_path) {
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
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = VK_FALSE;
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

    VkPipelineColorBlendStateCreateInfo color_blending{};
    color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blending.attachmentCount = 1;
    color_blending.pAttachments = &blend;

    VkPipelineRenderingCreateInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_info.colorAttachmentCount = 1;
    rendering_info.pColorAttachmentFormats = &color_format;

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

void UiRenderer::create(Device& device, const PassResources& resources,
                        VkFormat output_format) {
    device_ = device.handle();
    physical_device_ = device.physical_device();
    global_set_layout_ = resources.global_set_layout;
    bindless_set_layout_ = resources.material_set_layout;
    color_format_ = (output_format != VK_FORMAT_UNDEFINED) ? output_format : resources.color_format;

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

    // Quad pipeline
    pipeline_ = create_ui_pipeline(device_, layout_, color_format_,
        base_path::resolve("shaders/ui_quad.vert.spv"),
        base_path::resolve("shaders/ui_quad.frag.spv"));

    // Text pipeline
    text_pipeline_ = create_ui_pipeline(device_, layout_, color_format_,
        base_path::resolve("shaders/ui_text.vert.spv"),
        base_path::resolve("shaders/ui_text.frag.spv"));

    // Descriptor pool: 2 sets × 2 SSBOs each = 4 storage buffer descriptors
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = 4;

    VkDescriptorPoolCreateInfo desc_pool_ci{};
    desc_pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    desc_pool_ci.maxSets = 2;
    desc_pool_ci.poolSizeCount = 1;
    desc_pool_ci.pPoolSizes = &pool_size;
    if (vkCreateDescriptorPool(device_, &desc_pool_ci, nullptr, &pool_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create UI descriptor pool");
    }

    // Double-buffered SSBOs + descriptor sets
    for (uint32_t i = 0; i < 2; ++i) {
        quad_ssbos_[i] = std::make_unique<Buffer>(
            device_, physical_device_, SSBO_SIZE,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        quad_ssbos_[i]->map();

        text_ssbos_[i] = std::make_unique<Buffer>(
            device_, physical_device_, TEXT_SSBO_SIZE,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
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
    safe_destroy(text_pipeline_, vkDestroyPipeline);
    safe_destroy(pipeline_, vkDestroyPipeline);
    safe_destroy(layout_, vkDestroyPipelineLayout);
    safe_destroy(quad_set_layout_, vkDestroyDescriptorSetLayout);
}

void UiRenderer::update(const UiNode& root, float viewport_width, float viewport_height,
                         uint32_t frame_index) {
    std::vector<GpuUiQuad> quads;
    collect_quads(root, quads, viewport_width, viewport_height);

    uint32_t count = std::min(static_cast<uint32_t>(quads.size()), MAX_UI_QUADS);
    quad_counts_[frame_index] = count;

    if (count > 0) {
        auto* dst = static_cast<GpuUiQuad*>(quad_ssbos_[frame_index]->mapped());
        std::memcpy(dst, quads.data(), count * sizeof(GpuUiQuad));
    }

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
    if (node.layout_rect.w <= 0 || node.layout_rect.h <= 0) return;

    const auto& s = node.style;
    bool has_bg = s.background_color.a > 0.0f || s.gradient.active();
    bool has_border = s.border_width > 0.0f && s.border_color.a > 0.0f;
    bool has_shadow = s.box_shadow.active();

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
        quads.push_back(shadow);
    }

    if (has_bg || has_border) {
        // flags: 0 = normal, 1 = gradient
        float flags = s.gradient.active() ? 1.0f : 0.0f;

        GpuUiQuad quad{};
        quad.rect = {node.layout_rect.x, node.layout_rect.y,
                     node.layout_rect.w, node.layout_rect.h};
        quad.color = {s.background_color.r, s.background_color.g,
                      s.background_color.b, s.background_color.a};
        quad.border_color = {s.border_color.r, s.border_color.g,
                             s.border_color.b, s.border_color.a};
        quad.corners = {s.border_radius.top_left, s.border_radius.top_right,
                        s.border_radius.bottom_right, s.border_radius.bottom_left};
        quad.params = {s.border_width, s.opacity, flags, s.gradient.angle};
        quad.clip_rect = {node.clip_rect.x, node.clip_rect.y,
                          node.clip_rect.w, node.clip_rect.h};
        if (s.gradient.active()) {
            // For gradient: color = from, gradient_to = to
            quad.color = {s.gradient.from.r, s.gradient.from.g,
                          s.gradient.from.b, s.gradient.from.a};
            quad.gradient_to = {s.gradient.to.r, s.gradient.to.g,
                                s.gradient.to.b, s.gradient.to.a};
        }
        quad.viewport_size = {viewport_width, viewport_height};
        quads.push_back(quad);
    }

    for (const auto& child : node.children) {
        collect_quads(*child, quads, viewport_width, viewport_height);
    }
}

void UiRenderer::record(VkCommandBuffer cmd, VkDescriptorSet global_set, VkDescriptorSet bindless_set,
                         VkExtent2D extent, uint32_t frame_index) {
    bool has_quads = quad_counts_[frame_index] > 0;
    bool has_text = text_counts_[frame_index] > 0;
    if (!has_quads && !has_text) return;

    VkViewport vp{};
    vp.width = static_cast<float>(extent.width);
    vp.height = static_cast<float>(extent.height);
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D sc{};
    sc.extent = extent;
    vkCmdSetScissor(cmd, 0, 1, &sc);

    // Bind shared descriptor sets
    VkDescriptorSet sets[] = {global_set, bindless_set, quad_sets_[frame_index]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_,
                            0, 3, sets, 0, nullptr);

    // Draw quads
    if (has_quads) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        vkCmdDraw(cmd, quad_counts_[frame_index] * 6, 1, 0, 0);
    }

    // Draw text glyphs
    if (has_text) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, text_pipeline_);
        vkCmdDraw(cmd, text_counts_[frame_index] * 6, 1, 0, 0);
    }
}

} // namespace fjell
