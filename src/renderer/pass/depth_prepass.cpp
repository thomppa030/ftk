#include "renderer/pass/depth_prepass.hpp"
#include "core/profiler.hpp"
#include "renderer/pass/mesh_shader_depth_pass.hpp"
#include "renderer/gpu/vk_utils.hpp"
#include "renderer/pass/terrain_pass.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/indirect_batch.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/resources/mega_buffer.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass_registry.hpp"
#include "renderer/render_queue.hpp"
#include "renderer/resource_registry.hpp"
#include "renderer/systems/render_subsystem.hpp"
#include "renderer/resources/skeletal_model.hpp"
#include "renderer/gpu/shader_utils.hpp"
#include "renderer/resources/vertex.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#ifdef FJELL_ENABLE_TRACY
#include <tracy/TracyVulkan.hpp>
#endif

#include <glm/glm.hpp>

#include <array>
namespace fjell {

Result<> DepthPrePass::create(Device& device, const PassResources& resources) {
    device_ = device.handle();
    vk_pipeline_cache_ = resources.pipeline_cache;
    draw_data_layout_ = resources.draw_data_layout;

    // Store config for creating custom pipelines later
    depth_config_.depth_format = resources.depth_format;
    depth_config_.samples = resources.sample_count;
    depth_config_.depth_compare = VK_COMPARE_OP_LESS;

    auto vert_code = read_spirv(base_path::resolve("shaders/depth.vert.spv"));
    auto frag_code = read_spirv(base_path::resolve("shaders/depth_only.frag.spv"));
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

    std::array dynamic_states = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};

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
    multisampling.rasterizationSamples = resources.sample_count;

    VkPipelineColorBlendStateCreateInfo color_blending{};
    color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blending.logicOpEnable = VK_FALSE;
    color_blending.attachmentCount = 0;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_TRUE;
    depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS;
    depth_stencil.depthBoundsTestEnable = VK_FALSE;
    depth_stencil.stencilTestEnable = VK_FALSE;

    std::array set_layouts = {resources.global_set_layout, resources.material_set_layout, draw_data_layout_};

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = static_cast<uint32_t>(set_layouts.size());
    layout_info.pSetLayouts = set_layouts.data();
    layout_info.pushConstantRangeCount = 0;

    if (vkCreatePipelineLayout(device_, &layout_info, nullptr, &layout_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, vert_module, nullptr);
        vkDestroyShaderModule(device_, frag_module, nullptr);
        return make_error("Failed to create depth prepass pipeline layout");
    }

    depth_config_.layout = layout_;
    depth_config_.pipeline_cache = vk_pipeline_cache_;

    VkPipelineRenderingCreateInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_info.colorAttachmentCount = 0;
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
        return make_error("Failed to create depth prepass pipeline");
    }

    // Skinned depth prepass pipeline
    auto skinned_vert_code = read_spirv(base_path::resolve("shaders/skinned_depth.vert.spv"));
    auto skinned_vert_module = create_shader_module(device_, skinned_vert_code);

    stages[0].module = skinned_vert_module;

    auto skinned_binding_desc = SkinnedVertex::get_binding_description();
    auto skinned_attrib_descs = SkinnedVertex::get_attribute_descriptions();

    VkPipelineVertexInputStateCreateInfo skinned_vertex_input{};
    skinned_vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    skinned_vertex_input.vertexBindingDescriptionCount = 1;
    skinned_vertex_input.pVertexBindingDescriptions = &skinned_binding_desc;
    skinned_vertex_input.vertexAttributeDescriptionCount = static_cast<uint32_t>(skinned_attrib_descs.size());
    skinned_vertex_input.pVertexAttributeDescriptions = skinned_attrib_descs.data();

    pipeline_info.pStages = stages.data();
    pipeline_info.pVertexInputState = &skinned_vertex_input;

    if (vkCreateGraphicsPipelines(device_, vk_pipeline_cache_, 1, &pipeline_info,
                                   nullptr, &skinned_pipeline_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, skinned_vert_module, nullptr);
        vkDestroyShaderModule(device_, vert_module, nullptr);
        vkDestroyShaderModule(device_, frag_module, nullptr);
        return make_error("Failed to create skinned depth prepass pipeline");
    }

    vkDestroyShaderModule(device_, skinned_vert_module, nullptr);
    vkDestroyShaderModule(device_, vert_module, nullptr);
    vkDestroyShaderModule(device_, frag_module, nullptr);
    FJELL_GFX_DEBUG("Depth prepass pipelines created (standard + skinned)");
    return {};
}

void DepthPrePass::register_custom_depth(uint32_t pipeline_id,
                                          const std::string& vert_spv_path) {
    auto pipeline = create_depth_vertex_pipeline(device_, vert_spv_path, depth_config_);
    register_pipeline(device_, custom_depth_pipelines_, pipeline_id, pipeline);
}

void DepthPrePass::destroy() {
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
}

void DepthPrePass::record(const FrameContext& ctx) {
    FJELL_PROFILE_SCOPE_N("record_depth_prepass");
    if (ctx.passes != nullptr) {
        if (auto* msd = ctx.passes->try_get<MeshShaderDepthPass>("mesh_shader_depth")) {
            msd->record(ctx);
            return;
        }
    }

    if (ctx.target == nullptr || ctx.queue == nullptr) return;
    // Terrain is mesh-shader generated and never enters the render queue, so
    // an empty queue can still have depth to write. See the mesh-shader path.
    // With nothing at all to draw the clear still has to happen: particles
    // and the sky test against this depth, and an empty scene is a real
    // scene (an effect preview, a thumbnail).
    if (ctx.queue->empty() && !TerrainPass::scene_has_terrain(ctx)) {
        vk_utils::clear_depth_attachment(ctx.cmd, ctx.target->msaa_depth_view(),
                                         ctx.target->extent());
        return;
    }

    auto extent = ctx.target->extent();

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor{};
    scissor.offset = {0, 0};
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
    rendering_info.renderArea.offset = {0, 0};
    rendering_info.renderArea.extent = extent;
    rendering_info.layerCount = 1;
    rendering_info.colorAttachmentCount = 0;
    rendering_info.pDepthAttachment = &depth_attachment;

    vkCmdBeginRendering(ctx.cmd, &rendering_info);

    vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdSetViewport(ctx.cmd, 0, 1, &viewport);
    vkCmdSetScissor(ctx.cmd, 0, 1, &scissor);

    std::array descriptor_sets = {ctx.global_descriptor_set, ctx.bindless_set, ctx.draw_data_set};
    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_,
                            0, static_cast<uint32_t>(descriptor_sets.size()),
                            descriptor_sets.data(), 0, nullptr);

    if (ctx.mega_buffer && ctx.indirect_batch) {
        ctx.mega_buffer->bind(ctx.cmd);

        draw_indirect_per_range(ctx.cmd, pipeline_, custom_depth_pipelines_,
                                *ctx.indirect_batch, ctx.frame_index,
                                [&](VkCommandBuffer cmd) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_,
                                    0, static_cast<uint32_t>(descriptor_sets.size()),
                                    descriptor_sets.data(), 0, nullptr);
        });
    }

    // Skinned depth draws
    if (ctx.indirect_batch && ctx.queue) {
        const auto& skinned_indices = ctx.indirect_batch->skinned_draw_indices(ctx.frame_index);
        if (!skinned_indices.empty()) {
            vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skinned_pipeline_);
            vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_,
                                    0, static_cast<uint32_t>(descriptor_sets.size()),
                                    descriptor_sets.data(), 0, nullptr);

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

    // Subsystem depth contributions (lit-opaque sprites, tilemaps, …).
    // Each subsystem owns its pipeline + descriptor binding; they share the
    // depth attachment we just bound.
    for (uint32_t i = 0; i < ctx.subsystem_count; ++i) {
        ctx.subsystems[i]->record_depth(ctx);
    }

    // Terrain generates its geometry in a mesh shader from a height texture, so
    // it has no draw in the render queue and has to record itself.
    if (ctx.passes != nullptr) {
        if (auto* terrain = ctx.passes->try_get<TerrainPass>("terrain")) {
            terrain->record_depth(ctx);
        }
    }

    vkCmdEndRendering(ctx.cmd);
}

PassSetup DepthPrePass::setup() const {
    return {
        .name = "depth_prepass",
        .phase = RenderPhase::DepthPrepass,
        .sub_order = 50,
    };
}

void DepthPrePass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    builder.parallel_group(1);
    const bool has_msaa = ctx.msaa_samples != VK_SAMPLE_COUNT_1_BIT;
    auto depth = builder.import_named(ctx, has_msaa ? "msaa_depth" : "depth");
    builder.write(depth, gpu::Access::depth_attachment);
    // On the mesh-shader path the prepass draws what the first meshlet cull
    // found visible. Without mesh shaders nothing exports these, and the
    // imports come back empty.
    builder.read(builder.import_named_buffer(ctx, "meshlet_visible"),
                 gpu::Access::storage_buffer_read_mesh);
    builder.read(builder.import_named_buffer(ctx, "meshlet_indirect"),
                 gpu::Access::indirect_read);
    // The vertex path draws the camera batch's commands, and its instanced
    // draws read the transforms the instance cull kept. The mesh path reads
    // the uploaded transforms instead.
    if (!ctx.mesh_shader_supported) {
        builder.read(builder.import_named_buffer(ctx, "instance_transforms"),
                     gpu::Access::storage_buffer_read_vertex);
        builder.read(builder.import_named_buffer(ctx, "batch_indirect"),
                     gpu::Access::indirect_read);
    }
}

} // namespace fjell
