#include "ui/game_ui/ui_renderer.hpp"
#include "ui/game_ui/ui_node.hpp"
#include "renderer/device.hpp"
#include "renderer/shader_utils.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <cstring>

namespace fjell {

void UiRenderer::create(Device& device, const PassResources& resources,
                        VkFormat output_format) {
    device_ = device.handle();
    physical_device_ = device.physical_device();
    global_set_layout_ = resources.global_set_layout;
    bindless_set_layout_ = resources.material_set_layout;
    color_format_ = (output_format != VK_FORMAT_UNDEFINED) ? output_format : resources.color_format;

    // UI quad SSBO descriptor set layout (set 2, binding 0)
    VkDescriptorSetLayoutBinding ssbo_binding{};
    ssbo_binding.binding = 0;
    ssbo_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ssbo_binding.descriptorCount = 1;
    ssbo_binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_ci.bindingCount = 1;
    layout_ci.pBindings = &ssbo_binding;
    if (vkCreateDescriptorSetLayout(device_, &layout_ci, nullptr, &quad_set_layout_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create UI quad set layout");
    }

    // Pipeline layout: set 0 = global, set 1 = bindless textures, set 2 = UI quad SSBO
    VkDescriptorSetLayout sets[] = {global_set_layout_, bindless_set_layout_, quad_set_layout_};
    VkPipelineLayoutCreateInfo pipe_layout_ci{};
    pipe_layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipe_layout_ci.setLayoutCount = 3;
    pipe_layout_ci.pSetLayouts = sets;
    if (vkCreatePipelineLayout(device_, &pipe_layout_ci, nullptr, &layout_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create UI pipeline layout");
    }

    // Shaders
    auto vert_mod = create_shader_module(device_, read_spirv(base_path::resolve("shaders/ui_quad.vert.spv")));
    auto frag_mod = create_shader_module(device_, read_spirv(base_path::resolve("shaders/ui_quad.frag.spv")));

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
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT; // UI renders post-resolve

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = VK_FALSE;
    depth_stencil.depthWriteEnable = VK_FALSE;

    // Premultiplied alpha blending
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

    // Dynamic rendering (no render pass objects)
    VkPipelineRenderingCreateInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_info.colorAttachmentCount = 1;
    rendering_info.pColorAttachmentFormats = &color_format_;

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
    ci.layout = layout_;

    if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create UI quad pipeline");
    }

    vkDestroyShaderModule(device_, vert_mod, nullptr);
    vkDestroyShaderModule(device_, frag_mod, nullptr);

    // Descriptor pool for 2 quad SSBO sets
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = 2;

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

        VkDescriptorSetAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool = pool_;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &quad_set_layout_;
        if (vkAllocateDescriptorSets(device_, &alloc, &quad_sets_[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to allocate UI descriptor set");
        }

        VkDescriptorBufferInfo buf_info{quad_ssbos_[i]->handle(), 0, SSBO_SIZE};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = quad_sets_[i];
        write.dstBinding = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.descriptorCount = 1;
        write.pBufferInfo = &buf_info;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }

    FJELL_GFX_DEBUG("UI renderer created");
}

void UiRenderer::destroy() {
    auto safe_destroy = [&](auto& handle, auto destroy_fn) {
        if (handle != VK_NULL_HANDLE) { destroy_fn(device_, handle, nullptr); handle = VK_NULL_HANDLE; }
    };

    for (auto& ssbo : quad_ssbos_) ssbo.reset();
    safe_destroy(pool_, vkDestroyDescriptorPool);
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
}

void UiRenderer::collect_quads(const UiNode& node, std::vector<GpuUiQuad>& quads,
                                float viewport_width, float viewport_height) {
    // Skip invisible nodes
    if (node.style.display == UiDisplay::none) return;
    if (node.layout_rect.w <= 0 || node.layout_rect.h <= 0) return;

    // Only emit a quad if the node has a visible background or border
    bool has_bg = node.style.background_color.a > 0.0f;
    bool has_border = node.style.border_width > 0.0f && node.style.border_color.a > 0.0f;
    if (has_bg || has_border) {
        GpuUiQuad quad{};
        quad.rect = {node.layout_rect.x, node.layout_rect.y,
                     node.layout_rect.w, node.layout_rect.h};
        quad.color = {node.style.background_color.r, node.style.background_color.g,
                      node.style.background_color.b, node.style.background_color.a};
        quad.border_color = {node.style.border_color.r, node.style.border_color.g,
                             node.style.border_color.b, node.style.border_color.a};
        quad.corners = {node.style.border_radius.top_left, node.style.border_radius.top_right,
                        node.style.border_radius.bottom_right, node.style.border_radius.bottom_left};
        quad.params = {node.style.border_width, node.style.opacity, 0.0f, 0.0f};
        quad.clip_rect = {node.clip_rect.x, node.clip_rect.y,
                          node.clip_rect.w, node.clip_rect.h};
        quad.viewport_size = {viewport_width, viewport_height};
        quads.push_back(quad);
    }

    // Recurse to children (front-to-back for correct painter's order)
    for (const auto& child : node.children) {
        collect_quads(*child, quads, viewport_width, viewport_height);
    }
}

void UiRenderer::record(VkCommandBuffer cmd, VkDescriptorSet global_set, VkDescriptorSet bindless_set,
                         VkExtent2D extent, uint32_t frame_index) {
    if (quad_counts_[frame_index] == 0) return;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

    VkViewport vp{};
    vp.width = static_cast<float>(extent.width);
    vp.height = static_cast<float>(extent.height);
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D sc{};
    sc.extent = extent;
    vkCmdSetScissor(cmd, 0, 1, &sc);

    // Bind set 0 = global, set 1 = bindless, set 2 = UI quad SSBO
    VkDescriptorSet sets[] = {global_set, bindless_set, quad_sets_[frame_index]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_,
                            0, 3, sets, 0, nullptr);

    vkCmdDraw(cmd, quad_counts_[frame_index] * 6, 1, 0, 0);
}

} // namespace fjell
