#include "renderer/pass/terrain_pass.hpp"

#include "core/base_path.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "renderer/components/terrain_component.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass/pass_gpu_zone.hpp"
#include "renderer/render_stats.hpp"
#include "renderer/resources/shader_utils.hpp"
#include "renderer/resources/terrain_asset.hpp"
#include "scene/node.hpp"
#include "scene/scene.hpp"

#include <array>

namespace fjell {

Result<> TerrainPass::create(Device& device, const PassResources& resources) {
    device_ = device.handle();
    draw_mesh_tasks_ = device.draw_mesh_tasks_fn();

    // The height texture lives on its own set: it changes with the terrain
    // asset, not with the frame, and the global set has no free binding for a
    // per-asset resource.
    VkDescriptorSetLayoutBinding height_binding{};
    height_binding.binding = 0;
    height_binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    height_binding.descriptorCount = 1;
    height_binding.stageFlags = VK_SHADER_STAGE_MESH_BIT_EXT;

    VkDescriptorSetLayoutCreateInfo set_layout_info{};
    set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout_info.bindingCount = 1;
    set_layout_info.pBindings = &height_binding;
    if (vkCreateDescriptorSetLayout(device_, &set_layout_info, nullptr,
                                    &height_set_layout_) != VK_SUCCESS) {
        return make_error("Failed to create terrain height set layout");
    }

    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = 1;

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    if (vkCreateDescriptorPool(device_, &pool_info, nullptr, &descriptor_pool_) !=
        VK_SUCCESS) {
        return make_error("Failed to create terrain descriptor pool");
    }

    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = descriptor_pool_;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &height_set_layout_;
    if (vkAllocateDescriptorSets(device_, &alloc_info, &height_set_) != VK_SUCCESS) {
        return make_error("Failed to allocate terrain descriptor set");
    }

    auto task_code = read_spirv(base_path::resolve("shaders/terrain.task.spv"));
    auto mesh_code = read_spirv(base_path::resolve("shaders/terrain.mesh.spv"));
    auto frag_code = read_spirv(base_path::resolve("shaders/terrain.frag.spv"));
    auto depth_mesh_code = read_spirv(base_path::resolve("shaders/terrain_depth.mesh.spv"));
    auto depth_frag_code = read_spirv(base_path::resolve("shaders/depth_only.frag.spv"));
    auto wire_frag_code = read_spirv(base_path::resolve("shaders/terrain_wire.frag.spv"));

    VkShaderModule task_module = create_shader_module(device_, task_code);
    VkShaderModule mesh_module = create_shader_module(device_, mesh_code);
    VkShaderModule frag_module = create_shader_module(device_, frag_code);
    VkShaderModule depth_mesh_module = create_shader_module(device_, depth_mesh_code);
    VkShaderModule depth_frag_module = create_shader_module(device_, depth_frag_code);
    VkShaderModule wire_frag_module = create_shader_module(device_, wire_frag_code);

    auto cleanup_modules = [&] {
        vkDestroyShaderModule(device_, wire_frag_module, nullptr);
        vkDestroyShaderModule(device_, depth_frag_module, nullptr);
        vkDestroyShaderModule(device_, depth_mesh_module, nullptr);
        vkDestroyShaderModule(device_, frag_module, nullptr);
        vkDestroyShaderModule(device_, mesh_module, nullptr);
        vkDestroyShaderModule(device_, task_module, nullptr);
    };

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
    rasterizer.lineWidth = 1.0F;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = resources.sample_count;

    VkPipelineColorBlendAttachmentState opaque_blend{};
    opaque_blend.blendEnable = VK_FALSE;
    opaque_blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendAttachmentState mr_blend = opaque_blend;
    VkPipelineColorBlendAttachmentState normal_blend = opaque_blend;
    VkPipelineColorBlendAttachmentState velocity_blend = opaque_blend;

    std::array blend_attachments = {opaque_blend, mr_blend, normal_blend, velocity_blend};
    VkPipelineColorBlendStateCreateInfo color_blending{};
    color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blending.attachmentCount = static_cast<uint32_t>(blend_attachments.size());
    color_blending.pAttachments = blend_attachments.data();

    // Depth is already written by the prepass, which draws this same surface
    // from the same height texture. Testing LESS against our own prepass depth
    // rejects every fragment, so the color pass tests equal-or-nearer and does
    // not write — the same arrangement every other opaque pipeline uses.
    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_FALSE;
    depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    std::array<VkDescriptorSetLayout, 2> set_layouts = {resources.global_set_layout,
                                                        height_set_layout_};

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT;
    push_range.offset = 0;
    push_range.size = sizeof(TerrainPushConstants);

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = static_cast<uint32_t>(set_layouts.size());
    layout_info.pSetLayouts = set_layouts.data();
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_range;

    if (vkCreatePipelineLayout(device_, &layout_info, nullptr, &layout_) != VK_SUCCESS) {
        cleanup_modules();
        return make_error("Failed to create terrain pipeline layout");
    }

    std::array<VkFormat, 4> color_formats = {resources.color_format, VK_FORMAT_R8G8_UNORM,
                                             OffscreenPass::NORMAL_FORMAT,
                                             OffscreenPass::VELOCITY_FORMAT};

    VkPipelineRenderingCreateInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_info.colorAttachmentCount = static_cast<uint32_t>(color_formats.size());
    rendering_info.pColorAttachmentFormats = color_formats.data();
    rendering_info.depthAttachmentFormat = resources.depth_format;

    // No vertex input or input assembly state: a mesh shader pipeline generates
    // its own primitives, and Vulkan forbids supplying either here.
    VkGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.pNext = &rendering_info;
    pipeline_info.stageCount = static_cast<uint32_t>(stages.size());
    pipeline_info.pStages = stages.data();
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterizer;
    pipeline_info.pMultisampleState = &multisampling;
    pipeline_info.pDepthStencilState = &depth_stencil;
    pipeline_info.pColorBlendState = &color_blending;
    pipeline_info.pDynamicState = &dynamic_state;
    pipeline_info.layout = layout_;
    pipeline_info.renderPass = VK_NULL_HANDLE;

    if (vkCreateGraphicsPipelines(device_, resources.pipeline_cache, 1, &pipeline_info,
                                  nullptr, &pipeline_) != VK_SUCCESS) {
        cleanup_modules();
        return make_error("Failed to create terrain pipeline");
    }

    // Depth-only variant for the prepass: same task shader and push constants,
    // a mesh shader that emits position alone, and no color attachments. It
    // keeps an empty fragment stage like the other depth-only pipelines:
    // without one the pipeline has no fragment output interface state, so
    // pMultisampleState is dropped and the validation layer reports it NULL
    // (VUID-VkGraphicsPipelineCreateInfo-pMultisampleState-09026).
    std::array<VkPipelineShaderStageCreateInfo, 3> depth_stages = stages;
    depth_stages[1].module = depth_mesh_module;
    depth_stages[2].module = depth_frag_module;

    VkPipelineColorBlendStateCreateInfo depth_blending{};
    depth_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    depth_blending.attachmentCount = 0;

    VkPipelineRenderingCreateInfo depth_rendering{};
    depth_rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    depth_rendering.colorAttachmentCount = 0;
    depth_rendering.depthAttachmentFormat = resources.depth_format;

    // The prepass variant is the one that actually writes depth.
    VkPipelineDepthStencilStateCreateInfo depth_write = depth_stencil;
    depth_write.depthWriteEnable = VK_TRUE;
    depth_write.depthCompareOp = VK_COMPARE_OP_LESS;

    VkGraphicsPipelineCreateInfo depth_info = pipeline_info;
    depth_info.pNext = &depth_rendering;
    depth_info.pDepthStencilState = &depth_write;
    depth_info.stageCount = static_cast<uint32_t>(depth_stages.size());
    depth_info.pStages = depth_stages.data();
    depth_info.pColorBlendState = &depth_blending;

    if (vkCreateGraphicsPipelines(device_, resources.pipeline_cache, 1, &depth_info,
                                  nullptr, &depth_pipeline_) != VK_SUCCESS) {
        cleanup_modules();
        return make_error("Failed to create terrain depth pipeline");
    }

    // Patch grid overlay: same task/mesh shaders, a flat-colour fragment shader
    // so the lines read against the surface instead of vanishing into its own
    // shading. No culling, so both sides of a ridge show.
    VkPipelineRasterizationStateCreateInfo wire_raster = rasterizer;
    wire_raster.polygonMode = VK_POLYGON_MODE_LINE;
    wire_raster.cullMode = VK_CULL_MODE_NONE;
    wire_raster.lineWidth = 1.0F;

    std::array<VkPipelineShaderStageCreateInfo, 3> wire_stages = stages;
    wire_stages[2].module = wire_frag_module;

    // Colour only. GTAO, SSR and TAA read the other targets, and an editor
    // overlay must not disturb what the fill pass wrote into them.
    VkPipelineColorBlendAttachmentState wire_color = opaque_blend;
    VkPipelineColorBlendAttachmentState wire_masked{};
    wire_masked.blendEnable = VK_FALSE;
    wire_masked.colorWriteMask = 0;

    std::array wire_blend_attachments = {wire_color, wire_masked, wire_masked, wire_masked};
    VkPipelineColorBlendStateCreateInfo wire_blending = color_blending;
    wire_blending.attachmentCount = static_cast<uint32_t>(wire_blend_attachments.size());
    wire_blending.pAttachments = wire_blend_attachments.data();

    VkGraphicsPipelineCreateInfo wire_info = pipeline_info;
    wire_info.pRasterizationState = &wire_raster;
    wire_info.pStages = wire_stages.data();
    wire_info.pColorBlendState = &wire_blending;

    if (vkCreateGraphicsPipelines(device_, resources.pipeline_cache, 1, &wire_info,
                                  nullptr, &wireframe_pipeline_) != VK_SUCCESS) {
        cleanup_modules();
        return make_error("Failed to create terrain wireframe pipeline");
    }

    cleanup_modules();
    FJELL_GFX_DEBUG("Terrain pass pipelines created (color + depth + wireframe)");
    return {};
}

void TerrainPass::destroy() {
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (depth_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, depth_pipeline_, nullptr);
        depth_pipeline_ = VK_NULL_HANDLE;
    }
    if (wireframe_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, wireframe_pipeline_, nullptr);
        wireframe_pipeline_ = VK_NULL_HANDLE;
    }
    if (layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, layout_, nullptr);
        layout_ = VK_NULL_HANDLE;
    }
    if (descriptor_pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
        descriptor_pool_ = VK_NULL_HANDLE;
        height_set_ = VK_NULL_HANDLE;
    }
    if (height_set_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, height_set_layout_, nullptr);
        height_set_layout_ = VK_NULL_HANDLE;
    }
    bound_terrain_ = nullptr;
}

void TerrainPass::bind_height_texture(const TerrainAsset& terrain) {
    if (bound_terrain_ == &terrain) return;

    VkDescriptorImageInfo image_info{};
    image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    image_info.imageView = terrain.height_view();
    image_info.sampler = terrain.height_sampler();

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = height_set_;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image_info;

    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    bound_terrain_ = &terrain;
}

bool TerrainPass::scene_has_terrain(const FrameContext& ctx) {
    if (ctx.scene == nullptr) {
        return false;
    }
    for (const auto& node : ctx.scene->objects()) {
        const auto* tc = node->components.get<TerrainComponent>();
        if (tc != nullptr && tc->terrain != nullptr) {
            return true;
        }
    }
    return false;
}

void TerrainPass::draw_terrain_nodes(const FrameContext& ctx, VkPipeline pipeline,
                                     VkPipelineLayout layout, bool count_draws,
                                     WireframeFilter filter) {
    auto extent = ctx.target->extent();
    VkViewport vp{};
    vp.width = static_cast<float>(extent.width);
    vp.height = static_cast<float>(extent.height);
    vp.maxDepth = 1.0F;
    VkRect2D sc{};
    sc.extent = extent;

    bool pipeline_bound = false;

    for (const auto& node : ctx.scene->objects()) {
        const auto* terrain_comp = node->components.get<TerrainComponent>();
        if (terrain_comp == nullptr || terrain_comp->terrain == nullptr) {
            continue;
        }
        if (filter == WireframeFilter::only_wireframe && !terrain_comp->show_wireframe) {
            continue;
        }

        const TerrainAsset& terrain = *terrain_comp->terrain;
        const uint32_t patches = terrain.patches_per_side();
        if (patches == 0) {
            continue;
        }

        bind_height_texture(terrain);

        if (!pipeline_bound) {
            vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            vkCmdSetViewport(ctx.cmd, 0, 1, &vp);
            vkCmdSetScissor(ctx.cmd, 0, 1, &sc);
            pipeline_bound = true;
        }

        std::array<VkDescriptorSet, 2> sets = {ctx.global_descriptor_set, height_set_};
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0,
                                static_cast<uint32_t>(sets.size()), sets.data(), 0,
                                nullptr);

        // The node's transform positions the terrain; only its XZ translation
        // is used, since the heightfield carries its own vertical placement.
        const auto* spatial = node->as_spatial();
        const glm::vec3 origin =
            spatial != nullptr ? spatial->transform.position : glm::vec3{0.0F};

        TerrainPushConstants push{};
        push.world_origin = {origin.x, origin.z};
        push.world_size = terrain.world_size();
        push.height_scale = terrain.height_scale();
        push.height_offset = terrain.height_offset() + origin.y;
        push.patches_per_side = patches;
        push.patch_quads = terrain.patch_quads();
        vkCmdPushConstants(ctx.cmd, layout,
                           VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT, 0,
                           sizeof(push), &push);

        draw_mesh_tasks_(ctx.cmd, patches * patches, 1, 1);

        if (count_draws && ctx.render_stats != nullptr) {
            ctx.render_stats->draw_calls++;
        }
    }
}

void TerrainPass::record(const FrameContext& ctx) {
    if (pipeline_ == VK_NULL_HANDLE || ctx.scene == nullptr || ctx.target == nullptr) {
        return;
    }

    FJELL_PROFILE_SCOPE_N("record_terrain");
    FJELL_GPU_ZONE(ctx, "Terrain");

    draw_terrain_nodes(ctx, pipeline_, layout_, true);

    // Patch grid overlay, drawn per node that asks for it. A second pass rather
    // than a branch inside the first: the lines sit on top of the shaded
    // surface, so they are a separate draw regardless.
    if (wireframe_pipeline_ != VK_NULL_HANDLE) {
        draw_terrain_nodes(ctx, wireframe_pipeline_, layout_, false,
                           TerrainPass::WireframeFilter::only_wireframe);
    }
}

void TerrainPass::record_depth(const FrameContext& ctx) {
    if (depth_pipeline_ == VK_NULL_HANDLE || ctx.scene == nullptr ||
        ctx.target == nullptr) {
        return;
    }

    FJELL_PROFILE_SCOPE_N("record_terrain_depth");

    // Depth draws are not counted: they render the same surface the color pass
    // already reports, and double-counting would misread the stats panel.
    draw_terrain_nodes(ctx, depth_pipeline_, layout_, false);
}

} // namespace fjell
