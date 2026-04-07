#include "renderer/gpu/pipeline.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/resources/shader_utils.hpp"
#include "renderer/uniform_types.hpp"
#include "renderer/resources/vertex.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#include <glm/glm.hpp>

#include <array>
#include <stdexcept>

namespace fjell {

Pipeline::Pipeline(Device& device,
                   VkFormat color_format, VkFormat depth_format,
                   VkDescriptorSetLayout global_layout,
                   VkDescriptorSetLayout material_layout,
                   VkDescriptorSetLayout draw_data_layout,
                   VkSampleCountFlagBits samples,
                   const std::string& vert_shader,
                   const std::string& frag_shader,
                   VkPipelineCache pipeline_cache)
    : device_{device}
    , color_format_{color_format}
    , depth_format_{depth_format}
    , global_layout_{global_layout}
    , material_layout_{material_layout}
    , draw_data_layout_{draw_data_layout}
    , samples_{samples}
    , vert_shader_{vert_shader}
    , frag_shader_{frag_shader}
    , vk_pipeline_cache_{pipeline_cache} {
    create();
}

Pipeline::~Pipeline() {
    destroy();
}

void Pipeline::recreate() {
    vkDeviceWaitIdle(device_.handle());
    destroy();
    create();
    FJELL_GFX_INFO("Pipeline recreated");
}

Result<> Pipeline::recreate_checked() {
    vkDeviceWaitIdle(device_.handle());
    destroy();
    auto result = create_impl();
    if (result) FJELL_GFX_INFO("Pipeline recreated");
    return result;
}

void Pipeline::create() {
    auto result = create_impl();
    if (!result) throw std::runtime_error(result.error());
}

Result<> Pipeline::create_impl() {
    auto vert_code = read_spirv(base_path::resolve(vert_shader_));
    auto frag_code = read_spirv(base_path::resolve(frag_shader_));

    VkShaderModule vert_module = fjell::create_shader_module(device_.handle(), vert_code);
    VkShaderModule frag_module = fjell::create_shader_module(device_.handle(), frag_code);

    VkPipelineShaderStageCreateInfo vert_stage{};
    vert_stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vert_stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vert_stage.module = vert_module;
    vert_stage.pName = "main";

    VkPipelineShaderStageCreateInfo frag_stage{};
    frag_stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    frag_stage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    frag_stage.module = frag_module;
    frag_stage.pName = "main";

    std::array stages = {vert_stage, frag_stage};

    auto binding_desc = Vertex::get_binding_description();
    auto attrib_descs = Vertex::get_attribute_descriptions();

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding_desc;
    vertex_input.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrib_descs.size());
    vertex_input.pVertexAttributeDescriptions = attrib_descs.data();

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    input_assembly.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    std::array dynamic_states = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };

    VkPipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
    dynamic_state.pDynamicStates = dynamic_states.data();

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = samples_;

    VkPipelineColorBlendAttachmentState color_blend_attachment{};
    color_blend_attachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    color_blend_attachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo color_blending{};
    color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blending.logicOpEnable = VK_FALSE;
    color_blending.attachmentCount = 1;
    color_blending.pAttachments = &color_blend_attachment;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_FALSE;
    depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    depth_stencil.depthBoundsTestEnable = VK_FALSE;
    depth_stencil.stencilTestEnable = VK_FALSE;

    std::array set_layouts = {global_layout_, material_layout_, draw_data_layout_};

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(PbrPushConstants);

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = static_cast<uint32_t>(set_layouts.size());
    layout_info.pSetLayouts = set_layouts.data();
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_range;

    if (vkCreatePipelineLayout(device_.handle(), &layout_info, nullptr, &layout_) != VK_SUCCESS) {
        return make_error("Failed to create pipeline layout");
    }

    // Dynamic rendering: specify attachment formats instead of render pass
    VkPipelineRenderingCreateInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_info.colorAttachmentCount = 1;
    rendering_info.pColorAttachmentFormats = &color_format_;
    rendering_info.depthAttachmentFormat = depth_format_;

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

    if (vkCreateGraphicsPipelines(device_.handle(), vk_pipeline_cache_, 1, &pipeline_info, nullptr, &pipeline_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_.handle(), frag_module, nullptr);
        vkDestroyShaderModule(device_.handle(), vert_module, nullptr);
        return make_error("Failed to create graphics pipeline");
    }

    vkDestroyShaderModule(device_.handle(), frag_module, nullptr);
    vkDestroyShaderModule(device_.handle(), vert_module, nullptr);

    FJELL_GFX_DEBUG("Graphics pipeline created");
    return {};
}

void Pipeline::destroy() {
    auto vk_device = device_.handle();
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk_device, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(vk_device, layout_, nullptr);
        layout_ = VK_NULL_HANDLE;
    }
}


} // namespace fjell
