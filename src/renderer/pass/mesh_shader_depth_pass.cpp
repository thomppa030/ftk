#include "renderer/pass/mesh_shader_depth_pass.hpp"
#include "renderer/device.hpp"
#include "renderer/indirect_batch.hpp"
#include "renderer/mega_buffer.hpp"
#include "renderer/meshlet_buffer.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/render_queue.hpp"
#include "renderer/shader_utils.hpp"
#include "renderer/skeletal_model.hpp"
#include "renderer/vertex.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#include <array>
#include <stdexcept>

namespace fjell {

void MeshShaderDepthPass::create(Device& device, const PassResources& resources) {
    device_ = device.handle();
    draw_mesh_tasks_ = device.draw_mesh_tasks_fn();
    draw_mesh_tasks_indirect_count_ = device.draw_mesh_tasks_indirect_count_fn();
    draw_data_layout_ = resources.draw_data_layout;
    meshlet_layout_ = resources.meshlet_layout;
    depth_format_ = resources.depth_format;
    sample_count_ = resources.sample_count;

    auto task_code = read_spirv(base_path::resolve("shaders/depth_mesh.task.spv"));
    auto mesh_code = read_spirv(base_path::resolve("shaders/depth_mesh.mesh.spv"));
    auto frag_code = read_spirv(base_path::resolve("shaders/depth_only.frag.spv"));

    auto task_module = create_shader_module(device_, task_code);
    auto mesh_module = create_shader_module(device_, mesh_code);
    auto frag_module = create_shader_module(device_, frag_code);

    std::array<VkPipelineShaderStageCreateInfo, 3> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_TASK_BIT_EXT;
    stages[0].module = task_module;
    stages[0].pName = "main";

    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_MESH_BIT_EXT;
    stages[1].module = mesh_module;
    stages[1].pName = "main";

    stages[2].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[2].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[2].module = frag_module;
    stages[2].pName = "main";

    // No vertex input for mesh shaders
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

    VkPipelineColorBlendStateCreateInfo color_blending{};
    color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blending.attachmentCount = 0;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_TRUE;
    depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS;

    // Pipeline layout: set 0 = global, set 1 = unused (material), set 2 = draw data, set 3 = meshlet
    std::array set_layouts = {resources.global_set_layout, resources.material_set_layout,
                              draw_data_layout_, meshlet_layout_};

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT;
    push_range.offset = 0;
    push_range.size = sizeof(uint32_t);

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = static_cast<uint32_t>(set_layouts.size());
    layout_info.pSetLayouts = set_layouts.data();
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_range;

    if (vkCreatePipelineLayout(device_, &layout_info, nullptr, &layout_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, task_module, nullptr);
        vkDestroyShaderModule(device_, mesh_module, nullptr);
        vkDestroyShaderModule(device_, frag_module, nullptr);
        throw std::runtime_error("Failed to create mesh shader depth pipeline layout");
    }

    VkPipelineRenderingCreateInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_info.colorAttachmentCount = 0;
    rendering_info.depthAttachmentFormat = resources.depth_format;

    VkGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.pNext = &rendering_info;
    pipeline_info.stageCount = static_cast<uint32_t>(stages.size());
    pipeline_info.pStages = stages.data();
    pipeline_info.pVertexInputState = nullptr;   // no vertex input for mesh shaders
    pipeline_info.pInputAssemblyState = nullptr; // no input assembly for mesh shaders
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterizer;
    pipeline_info.pMultisampleState = &multisampling;
    pipeline_info.pDepthStencilState = &depth_stencil;
    pipeline_info.pColorBlendState = &color_blending;
    pipeline_info.pDynamicState = &dynamic_state;
    pipeline_info.layout = layout_;

    if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info,
                                   nullptr, &pipeline_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, task_module, nullptr);
        vkDestroyShaderModule(device_, mesh_module, nullptr);
        vkDestroyShaderModule(device_, frag_module, nullptr);
        throw std::runtime_error("Failed to create mesh shader depth pipeline");
    }

    vkDestroyShaderModule(device_, task_module, nullptr);
    vkDestroyShaderModule(device_, mesh_module, nullptr);

    // Skinned depth pipeline (vertex shader fallback for skeletal meshes)
    auto skinned_vert_code = read_spirv(base_path::resolve("shaders/skinned_depth.vert.spv"));
    auto skinned_vert_module = create_shader_module(device_, skinned_vert_code);

    std::array<VkPipelineShaderStageCreateInfo, 2> skinned_stages{};
    skinned_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    skinned_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    skinned_stages[0].module = skinned_vert_module;
    skinned_stages[0].pName = "main";
    skinned_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    skinned_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    skinned_stages[1].module = frag_module;
    skinned_stages[1].pName = "main";

    auto skinned_binding = SkinnedVertex::get_binding_description();
    auto skinned_attribs = SkinnedVertex::get_attribute_descriptions();

    VkPipelineVertexInputStateCreateInfo skinned_vi{};
    skinned_vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    skinned_vi.vertexBindingDescriptionCount = 1;
    skinned_vi.pVertexBindingDescriptions = &skinned_binding;
    skinned_vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(skinned_attribs.size());
    skinned_vi.pVertexAttributeDescriptions = skinned_attribs.data();

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // Skinned depth layout: set 0 = global, set 1 = material, set 2 = draw data (with bones)
    std::array skinned_set_layouts = {resources.global_set_layout, resources.material_set_layout, draw_data_layout_};
    VkPipelineLayoutCreateInfo skinned_layout_info{};
    skinned_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    skinned_layout_info.setLayoutCount = static_cast<uint32_t>(skinned_set_layouts.size());
    skinned_layout_info.pSetLayouts = skinned_set_layouts.data();

    if (vkCreatePipelineLayout(device_, &skinned_layout_info, nullptr, &skinned_layout_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, skinned_vert_module, nullptr);
        vkDestroyShaderModule(device_, frag_module, nullptr);
        throw std::runtime_error("Failed to create skinned depth pipeline layout");
    }

    pipeline_info.stageCount = static_cast<uint32_t>(skinned_stages.size());
    pipeline_info.pStages = skinned_stages.data();
    pipeline_info.pVertexInputState = &skinned_vi;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.layout = skinned_layout_;

    if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info,
                                   nullptr, &skinned_pipeline_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, skinned_vert_module, nullptr);
        vkDestroyShaderModule(device_, frag_module, nullptr);
        throw std::runtime_error("Failed to create skinned depth pipeline");
    }

    vkDestroyShaderModule(device_, skinned_vert_module, nullptr);
    vkDestroyShaderModule(device_, frag_module, nullptr);
    FJELL_GFX_DEBUG("Mesh shader depth pass created");
}

void MeshShaderDepthPass::register_custom_depth(uint32_t pipeline_id,
                                                  const std::string& mesh_spv_path) {
    if (layout_ == VK_NULL_HANDLE) return;

    auto task_code = read_spirv(base_path::resolve("shaders/depth_mesh.task.spv"));
    auto mesh_code = read_spirv(mesh_spv_path);
    auto frag_code = read_spirv(base_path::resolve("shaders/depth_only.frag.spv"));

    auto task_module = create_shader_module(device_, task_code);
    auto mesh_module = create_shader_module(device_, mesh_code);
    auto frag_module = create_shader_module(device_, frag_code);

    std::array<VkPipelineShaderStageCreateInfo, 3> stages{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_TASK_BIT_EXT, task_module, "main", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_MESH_BIT_EXT, mesh_module, "main", nullptr};
    stages[2] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_FRAGMENT_BIT, frag_module, "main", nullptr};

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
    multisampling.rasterizationSamples = sample_count_;

    VkPipelineColorBlendStateCreateInfo color_blending{};
    color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blending.attachmentCount = 0;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_TRUE;
    depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineRenderingCreateInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_info.depthAttachmentFormat = depth_format_;

    VkGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.pNext = &rendering_info;
    pipeline_info.stageCount = static_cast<uint32_t>(stages.size());
    pipeline_info.pStages = stages.data();
    pipeline_info.pVertexInputState = nullptr;
    pipeline_info.pInputAssemblyState = nullptr;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterizer;
    pipeline_info.pMultisampleState = &multisampling;
    pipeline_info.pDepthStencilState = &depth_stencil;
    pipeline_info.pColorBlendState = &color_blending;
    pipeline_info.pDynamicState = &dynamic_state;
    pipeline_info.layout = layout_;

    VkPipeline custom_pipeline = VK_NULL_HANDLE;
    vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info,
                               nullptr, &custom_pipeline);
    register_pipeline(device_, custom_depth_pipelines_, pipeline_id, custom_pipeline);

    vkDestroyShaderModule(device_, task_module, nullptr);
    vkDestroyShaderModule(device_, mesh_module, nullptr);
    vkDestroyShaderModule(device_, frag_module, nullptr);
}

void MeshShaderDepthPass::destroy() {
    destroy_pipeline_map(device_, custom_depth_pipelines_);

    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (skinned_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, skinned_pipeline_, nullptr);
        skinned_pipeline_ = VK_NULL_HANDLE;
    }
    if (layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, layout_, nullptr);
        layout_ = VK_NULL_HANDLE;
    }
    if (skinned_layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, skinned_layout_, nullptr);
        skinned_layout_ = VK_NULL_HANDLE;
    }
}

void MeshShaderDepthPass::record(const FrameContext& ctx) {
    if (!ctx.target || !ctx.queue || ctx.queue->empty()) return;

    auto extent = ctx.target->extent();

    VkViewport viewport{};
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.maxDepth = 1.0f;

    VkRect2D scissor{};
    scissor.extent = extent;

    VkRenderingAttachmentInfo depth_attachment{};
    depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth_attachment.imageView = ctx.target->msaa_depth_view();
    depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth_attachment.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering_info.renderArea.extent = extent;
    rendering_info.layerCount = 1;
    rendering_info.pDepthAttachment = &depth_attachment;

    vkCmdBeginRendering(ctx.cmd, &rendering_info);

    vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdSetViewport(ctx.cmd, 0, 1, &viewport);
    vkCmdSetScissor(ctx.cmd, 0, 1, &scissor);

    std::array descriptor_sets = {ctx.global_descriptor_set, ctx.bindless_set,
                                  ctx.draw_data_set, ctx.meshlet_set};
    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_,
                            0, static_cast<uint32_t>(descriptor_sets.size()),
                            descriptor_sets.data(), 0, nullptr);

    // Indirect count dispatch: one task workgroup per draw, GPU reads command buffer
    if (ctx.indirect_batch && draw_mesh_tasks_indirect_count_) {
        VkPipeline bound_pipeline = pipeline_;
        const auto& ranges = ctx.indirect_batch->pipeline_ranges(ctx.frame_index);
        for (uint32_t r = 0; r < static_cast<uint32_t>(ranges.size()); ++r) {
            // Bind custom depth pipeline if this shader has vertex displacement
            auto cit = custom_depth_pipelines_.find(ranges[r].pipeline_id);
            VkPipeline needed = (cit != custom_depth_pipelines_.end()) ? cit->second : pipeline_;
            if (needed != bound_pipeline) {
                vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, needed);
                bound_pipeline = needed;
            }

            uint32_t first_draw = ranges[r].first_draw;
            vkCmdPushConstants(ctx.cmd, layout_,
                               VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT,
                               0, sizeof(uint32_t), &first_draw);

            VkDeviceSize cmd_offset = first_draw *
                                      sizeof(VkDrawMeshTasksIndirectCommandEXT);
            VkDeviceSize count_offset = r * sizeof(uint32_t);

            draw_mesh_tasks_indirect_count_(
                ctx.cmd,
                ctx.indirect_batch->mesh_indirect_buffer(ctx.frame_index),
                cmd_offset,
                ctx.indirect_batch->mesh_count_buffer(ctx.frame_index),
                count_offset,
                ranges[r].draw_count,
                sizeof(VkDrawMeshTasksIndirectCommandEXT));
        }
    }

    // TODO(compute-pre-skin): Remove vertex shader fallback once compute
    // pre-skin writes skinned vertices into the meshlet vertex SSBO.
    // Skinned depth draws (vertex shader fallback)
    if (ctx.indirect_batch && ctx.queue) {
        const auto& skinned_indices = ctx.indirect_batch->skinned_draw_indices(ctx.frame_index);
        if (!skinned_indices.empty()) {
            vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skinned_pipeline_);
            std::array skinned_sets = {ctx.global_descriptor_set, ctx.bindless_set, ctx.draw_data_set};
            vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skinned_layout_,
                                    0, static_cast<uint32_t>(skinned_sets.size()),
                                    skinned_sets.data(), 0, nullptr);

            uint32_t idx = 0;
            for (const auto& dc : ctx.queue->commands()) {
                if (!dc.skinned || !dc.skeletal_model) continue;
                if (idx >= skinned_indices.size()) break;
                dc.skeletal_model->bind(ctx.cmd);
                vkCmdDrawIndexed(ctx.cmd, dc.skeletal_model->index_count(),
                                 1, 0, 0, skinned_indices[idx]);
                idx++;
            }
        }
    }

    vkCmdEndRendering(ctx.cmd);
}

} // namespace fjell
