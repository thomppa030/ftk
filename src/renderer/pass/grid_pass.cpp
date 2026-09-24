#include "renderer/pass/grid_pass.hpp"
#include "core/profiler.hpp"
#include "renderer/pass/pass_gpu_zone.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/resources/shader_utils.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#include <array>
namespace fjell {

Result<> GridPass::create(Device& device, const PassResources& resources) {
    device_ = device.handle();
    vk_pipeline_cache_ = resources.pipeline_cache;

    auto vert_code = read_spirv(base_path::resolve("shaders/grid.vert.spv"));
    auto frag_code = read_spirv(base_path::resolve("shaders/grid.frag.spv"));

    auto vert_module = create_shader_module(device_, vert_code);
    auto frag_module = create_shader_module(device_, frag_code);

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert_module;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag_module;
    stages[1].pName = "main";

    // No vertex input — positions generated from gl_VertexIndex
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    input_assembly.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    std::array dynamic_states = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};

    VkPipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
    dynamic_state.pDynamicStates = dynamic_states.data();

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = resources.sample_count;

    // Alpha blending for grid fade
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.blendEnable = VK_TRUE;
    blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
    blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    blend_attachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo color_blending{};
    color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blending.attachmentCount = 1;
    color_blending.pAttachments = &blend_attachment;

    // The fragment shader writes gl_FragDepth from the ray-plane hit, so the
    // hardware test occludes the grid per sample against real geometry.
    // Depth writes stay off: the grid must not occlude anything after it.
    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_FALSE;
    depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkDescriptorSetLayout set_layout = resources.global_set_layout;

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(GridPushConstants);

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &set_layout;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_range;

    if (vkCreatePipelineLayout(device_, &layout_info, nullptr, &layout_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, vert_module, nullptr);
        vkDestroyShaderModule(device_, frag_module, nullptr);
        return make_error("Failed to create grid pipeline layout");
    }

    VkFormat color_format = resources.color_format;

    VkPipelineRenderingCreateInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_info.colorAttachmentCount = 1;
    rendering_info.pColorAttachmentFormats = &color_format;
    rendering_info.depthAttachmentFormat = resources.depth_format;

    VkGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.pNext = &rendering_info;
    pipeline_info.stageCount = static_cast<uint32_t>(stages.size());
    pipeline_info.pStages = stages.data();
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterizer;
    pipeline_info.pMultisampleState = &multisampling;
    pipeline_info.pDepthStencilState = &depth_stencil;
    pipeline_info.pColorBlendState = &color_blending;
    pipeline_info.pDynamicState = &dynamic_state;
    pipeline_info.layout = layout_;
    pipeline_info.renderPass = VK_NULL_HANDLE;

    if (vkCreateGraphicsPipelines(device_, vk_pipeline_cache_, 1, &pipeline_info,
                                   nullptr, &pipeline_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, vert_module, nullptr);
        vkDestroyShaderModule(device_, frag_module, nullptr);
        return make_error("Failed to create grid pipeline");
    }

    vkDestroyShaderModule(device_, frag_module, nullptr);
    vkDestroyShaderModule(device_, vert_module, nullptr);

    FJELL_GFX_DEBUG("Grid pass pipeline created");
    return {};
}

void GridPass::destroy() {
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, layout_, nullptr);
        layout_ = VK_NULL_HANDLE;
    }
}

PassSetup GridPass::setup() const {
    return {
        .name = "grid",
        .phase = RenderPhase::Color,
        .sub_order = 10, // after opaque (0), DDGI (5) and SS-GI (6),
                         // before screen color copy (25) and transparent (50)
    };
}

void GridPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    auto msaa_color = builder.import_named(ctx, "msaa_color");
    auto msaa_depth = builder.import_named(ctx, "msaa_depth");
    builder.write(msaa_color, ResourceAccess::color_attachment);
    // Depth-tested against scene geometry, but never written: the grid must
    // not occlude anything drawn after it.
    builder.read(msaa_depth, ResourceAccess::depth_attachment_read);
    // The backward cull walk starts at tonemap and follows readers of
    // `color`. With MSAA on, `msaa_color` is a distinct image nothing
    // samples, so the grid has no path back to a root and would be culled
    // despite being the whole point of the pass.
    builder.has_side_effects();
}

void GridPass::record(const FrameContext& ctx) {
    if (!ctx.show_grid || !ctx.target) return;

    FJELL_PROFILE_SCOPE_N("record_grid");
    FJELL_GPU_ZONE(ctx, "Grid");

    auto* target = ctx.target;
    auto extent = target->extent();

    // Load the color the opaque pass wrote and add to it. The resolve to
    // single-sample `color` belongs to the transparent pass, which renders
    // after this one.
    VkRenderingAttachmentInfo color_attachment{};
    color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color_attachment.imageView = target->msaa_color_view();
    color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingAttachmentInfo depth_attachment{};
    depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth_attachment.imageView = target->msaa_depth_view();
    depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.extent = extent;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color_attachment;
    ri.pDepthAttachment = &depth_attachment;

    vkCmdBeginRendering(ctx.cmd, &ri);

    VkViewport vp{};
    vp.width = static_cast<float>(extent.width);
    vp.height = static_cast<float>(extent.height);
    vp.maxDepth = 1.0f;

    VkRect2D sc{};
    sc.extent = extent;

    GridPushConstants push{};
    push.cell_size = settings_.cell_size;
    push.thickness = settings_.thickness;
    push.fade_distance = settings_.fade_distance;
    push.opacity = settings_.opacity;

    vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdSetViewport(ctx.cmd, 0, 1, &vp);
    vkCmdSetScissor(ctx.cmd, 0, 1, &sc);
    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_,
                            0, 1, &ctx.global_descriptor_set, 0, nullptr);
    vkCmdPushConstants(ctx.cmd, layout_,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(GridPushConstants), &push);
    vkCmdDraw(ctx.cmd, 3, 1, 0, 0);

    vkCmdEndRendering(ctx.cmd);
}

} // namespace fjell
