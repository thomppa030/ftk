#include "renderer/pass/transparent_pass.hpp"
#include "core/profiler.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/indirect_batch.hpp"
#include "renderer/resources/mega_buffer.hpp"
#include "renderer/resources/model.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/pass_registry.hpp"
#include "renderer/pass/water_pass.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/render_queue.hpp"
#include "renderer/resource_registry.hpp"
#include "renderer/resources/shader_utils.hpp"
#include "renderer/resources/skeletal_model.hpp"
#include "renderer/systems/render_subsystem.hpp"
#include "renderer/uniform_types.hpp"
#include "renderer/resources/vertex.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#ifdef FJELL_ENABLE_TRACY
#include <tracy/TracyVulkan.hpp>
#endif

#include <array>
namespace fjell {

Result<> TransparentPass::create(Device& device, const PassResources& resources) {
    device_ = device.handle();
    vk_pipeline_cache_ = resources.pipeline_cache;
    draw_data_layout_ = resources.draw_data_layout;

    // Same shaders as PBR — the alpha blending is in the pipeline state
    auto vert_code = read_spirv(base_path::resolve("shaders/mesh.vert.spv"));
    auto frag_code = read_spirv(base_path::resolve("shaders/mesh.frag.spv"));

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

    // Alpha blending: src * srcAlpha + dst * (1 - srcAlpha)
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendAttachmentState mr_blend{};
    mr_blend.colorWriteMask = 0; // Transparent objects don't write MR
    mr_blend.blendEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState normal_blend{};
    normal_blend.colorWriteMask = 0;
    normal_blend.blendEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState velocity_blend{};
    velocity_blend.colorWriteMask = 0;
    velocity_blend.blendEnable = VK_FALSE;

    std::array blend_attachments = {blend, mr_blend, normal_blend, velocity_blend};

    VkPipelineColorBlendStateCreateInfo color_blending{};
    color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blending.attachmentCount = static_cast<uint32_t>(blend_attachments.size());
    color_blending.pAttachments = blend_attachments.data();

    // Depth test yes, depth write no
    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_FALSE;
    depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    // Layout: global (set 0) + material (set 1) + draw data (set 2)
    std::array set_layouts = {resources.global_set_layout, resources.material_set_layout, draw_data_layout_};

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = static_cast<uint32_t>(set_layouts.size());
    layout_info.pSetLayouts = set_layouts.data();
    layout_info.pushConstantRangeCount = 0;

    if (vkCreatePipelineLayout(device_, &layout_info, nullptr, &layout_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, vert_module, nullptr);
        vkDestroyShaderModule(device_, frag_module, nullptr);
        return make_error("Failed to create transparent pipeline layout");
    }

    VkFormat depth_format = resources.depth_format;
    std::array<VkFormat, 4> color_formats = {resources.color_format, VK_FORMAT_R8G8_UNORM, OffscreenPass::NORMAL_FORMAT, OffscreenPass::VELOCITY_FORMAT};

    VkPipelineRenderingCreateInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_info.colorAttachmentCount = static_cast<uint32_t>(color_formats.size());
    rendering_info.pColorAttachmentFormats = color_formats.data();
    rendering_info.depthAttachmentFormat = depth_format;

    VkGraphicsPipelineCreateInfo pipeline_ci{};
    pipeline_ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_ci.pNext = &rendering_info;
    pipeline_ci.stageCount = static_cast<uint32_t>(stages.size());
    pipeline_ci.pStages = stages.data();
    pipeline_ci.pVertexInputState = &vertex_input;
    pipeline_ci.pInputAssemblyState = &input_assembly;
    pipeline_ci.pViewportState = &viewport_state;
    pipeline_ci.pRasterizationState = &rasterizer;
    pipeline_ci.pMultisampleState = &multisampling;
    pipeline_ci.pDepthStencilState = &depth_stencil;
    pipeline_ci.pColorBlendState = &color_blending;
    pipeline_ci.pDynamicState = &dynamic_state;
    pipeline_ci.layout = layout_;
    pipeline_ci.renderPass = VK_NULL_HANDLE;

    if (vkCreateGraphicsPipelines(device_, vk_pipeline_cache_, 1, &pipeline_ci,
                                   nullptr, &pipeline_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, vert_module, nullptr);
        vkDestroyShaderModule(device_, frag_module, nullptr);
        return make_error("Failed to create transparent pipeline");
    }

    vkDestroyShaderModule(device_, frag_module, nullptr);
    vkDestroyShaderModule(device_, vert_module, nullptr);

    FJELL_GFX_DEBUG("Transparent pass created");
    return {};
}

void TransparentPass::destroy() {
    if (pipeline_ != VK_NULL_HANDLE) { vkDestroyPipeline(device_, pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE; }
    if (layout_ != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device_, layout_, nullptr); layout_ = VK_NULL_HANDLE; }
}

void TransparentPass::record(const FrameContext& ctx) {
    FJELL_PROFILE_SCOPE_N("record_transparent");
#ifdef FJELL_ENABLE_TRACY
    TracyVkZone(static_cast<tracy::VkCtx*>(ctx.tracy_ctx), ctx.cmd, "Color Transparent");
#endif
    auto* target = ctx.target;
    if (target == nullptr) { return; }

    // Water before the transparent scope opens, not inside it. It is the
    // surface the rest of them float on or stand in, and it samples the opaque
    // scene behind it, which nothing drawn here has touched yet — so it still
    // goes first. What changed is that it now draws in a scope of its own,
    // against a depth attachment it may write, and a rendering scope cannot
    // nest inside another.
    //
    // The seed is a transfer and has to be outside a scope for that reason too.
    // Both do nothing when the scene holds no water.
    {
        auto& water = ctx.passes->get<WaterPass>("water");
        water.seed_water_depth(ctx);
        water.record(ctx);
    }

    VkRenderingAttachmentInfo color_attachment{};
    color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color_attachment.imageView = target->msaa_color_view();
    color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    bool msaa = target->sample_count() != VK_SAMPLE_COUNT_1_BIT;
    if (msaa) {
        color_attachment.resolveImageView = target->color_view();
        color_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_attachment.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
    }

    VkRenderingAttachmentInfo mr_attachment{};
    mr_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    mr_attachment.imageView = target->msaa_mr_view();
    mr_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    mr_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    mr_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    if (msaa) {
        mr_attachment.resolveImageView = target->mr_view();
        mr_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        mr_attachment.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
    }

    VkRenderingAttachmentInfo normal_attachment{};
    normal_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    normal_attachment.imageView = target->msaa_normal_view();
    normal_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    normal_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    normal_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    if (msaa) {
        normal_attachment.resolveImageView = target->normal_view();
        normal_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        normal_attachment.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
    }

    VkRenderingAttachmentInfo velocity_attachment{};
    velocity_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    velocity_attachment.imageView = target->msaa_velocity_view();
    velocity_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    velocity_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    velocity_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    if (msaa) {
        velocity_attachment.resolveImageView = target->velocity_view();
        velocity_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        velocity_attachment.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
    }

    std::array<VkRenderingAttachmentInfo, 4> color_attachments = {
        color_attachment, mr_attachment, normal_attachment, velocity_attachment};

    VkRenderingAttachmentInfo depth_attachment{};
    depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth_attachment.imageView = target->msaa_depth_view();
    depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
    if (msaa) {
        depth_attachment.resolveImageView = VK_NULL_HANDLE;
        depth_attachment.resolveMode = VK_RESOLVE_MODE_NONE;
    }

    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.extent = target->extent();
    ri.layerCount = 1;
    ri.colorAttachmentCount = static_cast<uint32_t>(color_attachments.size());
    ri.pColorAttachments = color_attachments.data();
    ri.pDepthAttachment = &depth_attachment;

    vkCmdBeginRendering(ctx.cmd, &ri);

    VkViewport vp{};
    vp.width = static_cast<float>(target->extent().width);
    vp.height = static_cast<float>(target->extent().height);
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(ctx.cmd, 0, 1, &vp);

    VkRect2D sc{};
    sc.extent = target->extent();
    vkCmdSetScissor(ctx.cmd, 0, 1, &sc);

    record_draws(ctx);

    // Worldspace UI: renders after transparent objects, before particles
    if (ctx.world_ui_callback && ctx.playing) {
        ctx.world_ui_callback(ctx.cmd,
            ctx.global_descriptor_set,
            ctx.bindless_set,
            target->extent(), ctx.frame_index);
    }

    for (uint32_t i = 0; i < ctx.subsystem_count; ++i) {
        ctx.subsystems[i]->record_transparent(ctx);
    }

    vkCmdEndRendering(ctx.cmd);
}

void TransparentPass::record_draws(const FrameContext& ctx) {
    if (!ctx.queue || !ctx.queue->has_transparent()) return;
    if (!ctx.indirect_batch || !ctx.mega_buffer) return;

    // Use pipeline cache transparent pipelines if available, otherwise fallback
    VkPipelineLayout active_layout = ctx.pipeline_cache
        ? ctx.pipeline_cache->layout() : layout_;

    uint32_t current_pipeline_id = UINT32_MAX;

    if (!ctx.pipeline_cache) {
        // Fallback: old single-pipeline transparent pass
        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        std::array descriptor_sets = {ctx.global_descriptor_set, ctx.bindless_set, ctx.draw_data_set};
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_,
                                0, static_cast<uint32_t>(descriptor_sets.size()),
                                descriptor_sets.data(), 0, nullptr);
        active_layout = layout_;
    }

    ctx.mega_buffer->bind(ctx.cmd);

    uint32_t base_index = ctx.indirect_batch->transparent_draw_base(ctx.frame_index);

    bool current_skinned = false;
    bool mega_bound = true;
    for (const auto& dc : ctx.queue->transparent_commands()) {
        // Switch pipeline if draw uses a different shader or skinned state changed
        if (ctx.pipeline_cache && (dc.pipeline_id != current_pipeline_id || dc.skinned != current_skinned)) {
            current_pipeline_id = dc.pipeline_id;
            current_skinned = dc.skinned;
            auto pipeline = current_skinned
                ? ctx.pipeline_cache->skinned_transparent_pipeline(current_pipeline_id)
                : ctx.pipeline_cache->transparent_pipeline(current_pipeline_id);
            vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            std::array descriptor_sets = {ctx.global_descriptor_set, ctx.bindless_set, ctx.draw_data_set};
            vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, active_layout,
                                    0, static_cast<uint32_t>(descriptor_sets.size()),
                                    descriptor_sets.data(), 0, nullptr);
        }

        if (dc.skinned) {
            if (dc.skeletal_model) {
                dc.skeletal_model->bind(ctx.cmd);
                vkCmdDrawIndexed(ctx.cmd, dc.skeletal_model->index_count(),
                                 1, 0, 0, base_index);
                mega_bound = false;
            }
            base_index++;
            continue;
        }
        if (!mega_bound) {
            ctx.mega_buffer->bind(ctx.cmd);
            mega_bound = true;
        }
        const auto& entries = dc.model->mega_entries();
        for (const auto& entry : entries) {
            vkCmdDrawIndexed(ctx.cmd, entry.index_count, 1,
                             entry.first_index,
                             static_cast<int32_t>(entry.vertex_offset),
                             base_index);
        }
        base_index++;
    }
}

PassSetup TransparentPass::setup() const {
    return {
        .name = "transparent",
        .phase = RenderPhase::Color,
        .sub_order = 50,
    };
}

void TransparentPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    auto msaa_color = builder.import_named(ctx, "msaa_color");
    auto color = builder.import_named(ctx, "color");
    auto msaa_mr = builder.import_named(ctx, "msaa_mr");
    auto mr = builder.import_named(ctx, "mr");
    auto msaa_normal = builder.import_named(ctx, "msaa_normal");
    auto normal = builder.import_named(ctx, "normal");
    auto msaa_velocity = builder.import_named(ctx, "msaa_velocity");
    auto velocity = builder.import_named(ctx, "velocity");
    auto msaa_depth = builder.import_named(ctx, "msaa_depth");
    auto depth = builder.import_named(ctx, "depth");
    auto screen = builder.import_named(ctx, "screen_color");

    builder.write(msaa_color, ResourceAccess::color_attachment);
    builder.write(color, ResourceAccess::color_attachment);
    builder.write(msaa_mr, ResourceAccess::color_attachment);
    builder.write(mr, ResourceAccess::color_attachment);
    builder.write(msaa_normal, ResourceAccess::color_attachment);
    builder.write(normal, ResourceAccess::color_attachment);
    builder.write(msaa_velocity, ResourceAccess::color_attachment);
    builder.write(velocity, ResourceAccess::color_attachment);
    builder.read(msaa_depth, ResourceAccess::sampled_fragment);
    builder.read(depth, ResourceAccess::sampled_fragment);
    builder.read(screen, ResourceAccess::sampled_fragment);

    if (auto atlas = builder.import_named(ctx, "shadow_atlas"); atlas.valid()) {
        builder.read(atlas, ResourceAccess::sampled_fragment);
    }
    if (auto atlas = builder.import_named(ctx, "local_shadow_atlas"); atlas.valid()) {
        builder.read(atlas, ResourceAccess::sampled_fragment);
    }
    if (auto cubemap = builder.import_named(ctx, "sky_cubemap"); cubemap.valid()) {
        builder.read(cubemap, ResourceAccess::sampled_fragment);
    }
}

} // namespace fjell
