#include "renderer/pass/terrain_pass.hpp"

#include "core/base_path.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "renderer/bindless.hpp"
#include "renderer/components/terrain_component.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/gpu/deferred_deleter.hpp"
#include "renderer/gpu_context.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass/pass_gpu_zone.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/render_stats.hpp"
#include "renderer/resources/material.hpp"
#include "renderer/resources/shader_utils.hpp"
#include "renderer/resources/terrain_asset.hpp"
#include "scene/node.hpp"
#include "scene/scene.hpp"

#include <array>
#include <span>
#include <vector>

namespace fjell {

namespace {

constexpr VkShaderStageFlags TERRAIN_PUSH_STAGES = VK_SHADER_STAGE_TASK_BIT_EXT |
                                                   VK_SHADER_STAGE_MESH_BIT_EXT |
                                                   VK_SHADER_STAGE_FRAGMENT_BIT;

// The engine's default terrain surface, drawn when a node has no material.
constexpr const char* FALLBACK_TERRAIN_SHADER = "shaders/terrain.fjsl";

} // namespace

Result<> TerrainPass::create(Device& device, const PassResources& resources) {
    device_ = device.handle();
    resources_ = resources;
    draw_mesh_tasks_ = device.draw_mesh_tasks_fn();

    // The terrain textures live on their own set: they change with the terrain
    // asset, not with the frame, and the global set has no free binding for a
    // per-asset resource. Heights feed the mesh stage, layer weights the
    // fragment stage.
    std::array<VkDescriptorSetLayoutBinding, 2> terrain_bindings{};
    terrain_bindings[0].binding = 0;
    terrain_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    terrain_bindings[0].descriptorCount = 1;
    terrain_bindings[0].stageFlags = VK_SHADER_STAGE_MESH_BIT_EXT;
    terrain_bindings[1].binding = 1;
    terrain_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    terrain_bindings[1].descriptorCount = 1;
    terrain_bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo set_layout_info{};
    set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout_info.bindingCount = static_cast<uint32_t>(terrain_bindings.size());
    set_layout_info.pBindings = terrain_bindings.data();
    if (vkCreateDescriptorSetLayout(device_, &set_layout_info, nullptr,
                                    &terrain_set_layout_) != VK_SUCCESS) {
        return make_error("Failed to create terrain set layout");
    }

    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = static_cast<uint32_t>(terrain_bindings.size());

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
    alloc_info.pSetLayouts = &terrain_set_layout_;
    if (vkAllocateDescriptorSets(device_, &alloc_info, &terrain_set_) != VK_SUCCESS) {
        return make_error("Failed to allocate terrain descriptor set");
    }

    auto task_code = read_spirv(base_path::resolve("shaders/terrain.task.spv"));
    auto mesh_code = read_spirv(base_path::resolve("shaders/terrain.mesh.spv"));
    auto depth_mesh_code = read_spirv(base_path::resolve("shaders/terrain_depth.mesh.spv"));
    auto depth_frag_code = read_spirv(base_path::resolve("shaders/depth_only.frag.spv"));
    auto wire_frag_code = read_spirv(base_path::resolve("shaders/terrain_wire.frag.spv"));

    // Task and mesh modules outlive create(): the colour pipelines are built
    // per terrain material, whenever one is first seen or recompiles.
    task_module_ = create_shader_module(device_, task_code);
    mesh_module_ = create_shader_module(device_, mesh_code);
    VkShaderModule depth_mesh_module = create_shader_module(device_, depth_mesh_code);
    VkShaderModule depth_frag_module = create_shader_module(device_, depth_frag_code);
    VkShaderModule wire_frag_module = create_shader_module(device_, wire_frag_code);

    auto cleanup_modules = [&] {
        vkDestroyShaderModule(device_, wire_frag_module, nullptr);
        vkDestroyShaderModule(device_, depth_frag_module, nullptr);
        vkDestroyShaderModule(device_, depth_mesh_module, nullptr);
    };

    std::array<VkPipelineShaderStageCreateInfo, 3> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_TASK_BIT_EXT;
    stages[0].module = task_module_;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_MESH_BIT_EXT;
    stages[1].module = mesh_module_;
    stages[1].pName = "main";
    stages[2].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[2].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[2].module = VK_NULL_HANDLE;   // per pipeline below
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

    // Set 1 is the material set — bindless textures, material rows, params —
    // that the FJSL fragment reads its layers and its own uniforms from.
    std::array<VkDescriptorSetLayout, 3> set_layouts = {resources.global_set_layout,
                                                        resources.material_set_layout,
                                                        terrain_set_layout_};

    VkPushConstantRange push_range{};
    push_range.stageFlags = TERRAIN_PUSH_STAGES;
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
    FJELL_GFX_DEBUG("Terrain pass pipelines created (depth + wireframe)");
    return {};
}

void TerrainPass::post_create(GpuContext& gpu, PassRegistry& /*passes*/) {
    pipeline_cache_ = gpu.pipeline_cache();
    bindless_ = &gpu.bindless_textures();
}

void TerrainPass::prepare_frame(const FrameContext& ctx, GpuContext& /*gpu*/) {
    if (ctx.scene == nullptr || pipeline_cache_ == nullptr) return;
    // Pipelines are built here rather than in record(): a first sighting of
    // a material or a hot-reloaded shader means vkCreateGraphicsPipelines,
    // which does not belong inside command recording.
    bool any = false;
    for (const auto& node : ctx.scene->objects()) {
        const auto* tc = node->components.get<TerrainComponent>();
        if (tc == nullptr || tc->terrain == nullptr) continue;
        any = true;
        if (tc->material != nullptr) {
            (void)ensure_material_pipeline(tc->material->pipeline_id());
        }
    }
    if (!any) return;
    ensure_fallback_material();
    // A hot-reloaded terrain.fjsl moves the generation of the fallback entry
    // like any other; keep its pipeline current too.
    if (fallback_ready_) {
        (void)ensure_material_pipeline(fallback_pipeline_id_);
    }
}

void TerrainPass::ensure_fallback_material() {
    if (fallback_ready_ || pipeline_cache_ == nullptr || bindless_ == nullptr) return;

    const uint32_t pid = pipeline_cache_->register_shader(FALLBACK_TERRAIN_SHADER);
    if (pipeline_cache_->has_compile_error(pid)) {
        FJELL_GFX_ERROR("TerrainPass: '{}' failed to compile: {}", FALLBACK_TERRAIN_SHADER,
                        pipeline_cache_->compile_error(pid));
        return;
    }
    const auto& meta = pipeline_cache_->metadata(pid);

    // A material row seeded with the shader's own defaults, as SkyPass does
    // for a sky shader with no .fjmat behind it.
    GpuMaterial gpu{};
    if (meta.custom_param_count > 0) {
        gpu.param_offset = bindless_->allocate_params(meta.custom_param_count);
        gpu.param_count = meta.custom_param_count;
        std::vector<float> seed(meta.custom_param_count, 0.0F);
        for (const auto& u : meta.uniforms) {
            if (u.type == UniformType::Sampler2D) continue;
            const uint32_t count = uniform_float_count(u.type);
            for (uint32_t i = 0; i < count && i < u.default_value.size(); ++i) {
                seed[u.slot + i] = u.default_value[i];
            }
            encode_uniform_for_gpu(u, std::span{seed}.subspan(u.slot, count));
        }
        bindless_->write_params(gpu.param_offset, seed.data(), meta.custom_param_count);
    }
    fallback_material_id_ = bindless_->add_material(gpu);
    fallback_pipeline_id_ = pid;
    fallback_ready_ = ensure_material_pipeline(pid) != VK_NULL_HANDLE;
}

VkPipeline TerrainPass::ensure_material_pipeline(uint32_t pipeline_id) {
    if (pipeline_cache_ == nullptr || pipeline_id >= pipeline_cache_->shader_count()) {
        return VK_NULL_HANDLE;
    }
    const uint32_t generation = pipeline_cache_->generation(pipeline_id);
    auto it = material_pipelines_.find(pipeline_id);
    if (it != material_pipelines_.end() && it->second.generation == generation) {
        return it->second.pipeline;
    }

    const auto& meta = pipeline_cache_->metadata(pipeline_id);
    if (meta.shader_type != ShaderType::Terrain) {
        if (reported_entries_.insert(pipeline_id).second) {
            FJELL_GFX_ERROR("TerrainPass: '{}' is shader_type '{}', a terrain needs "
                            "shader_type terrain",
                            pipeline_cache_->shader_path(pipeline_id), shader_type_name(meta.shader_type));
        }
        return VK_NULL_HANDLE;
    }
    if (pipeline_cache_->has_compile_error(pipeline_id) || generation == 0) {
        // A failed recompile keeps the pipeline built from the last good one.
        return it != material_pipelines_.end() ? it->second.pipeline : VK_NULL_HANDLE;
    }

    auto frag_code = read_spirv(pipeline_cache_->frag_spv_path(pipeline_id));
    VkShaderModule frag_module = create_shader_module(device_, frag_code);

    std::array<VkPipelineShaderStageCreateInfo, 3> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_TASK_BIT_EXT;
    stages[0].module = task_module_;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_MESH_BIT_EXT;
    stages[1].module = mesh_module_;
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
    multisampling.rasterizationSamples = resources_.sample_count;

    VkPipelineColorBlendAttachmentState opaque_blend{};
    opaque_blend.blendEnable = VK_FALSE;
    opaque_blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    std::array blend_attachments = {opaque_blend, opaque_blend, opaque_blend, opaque_blend};
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

    std::array<VkFormat, 4> color_formats = {resources_.color_format, VK_FORMAT_R8G8_UNORM,
                                             OffscreenPass::NORMAL_FORMAT,
                                             OffscreenPass::VELOCITY_FORMAT};
    VkPipelineRenderingCreateInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_info.colorAttachmentCount = static_cast<uint32_t>(color_formats.size());
    rendering_info.pColorAttachmentFormats = color_formats.data();
    rendering_info.depthAttachmentFormat = resources_.depth_format;

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

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result = vkCreateGraphicsPipelines(device_, resources_.pipeline_cache, 1,
                                                      &pipeline_info, nullptr, &pipeline);
    vkDestroyShaderModule(device_, frag_module, nullptr);
    if (result != VK_SUCCESS) {
        FJELL_GFX_ERROR("TerrainPass: failed to create pipeline for '{}'",
                        pipeline_cache_->shader_path(pipeline_id));
        return it != material_pipelines_.end() ? it->second.pipeline : VK_NULL_HANDLE;
    }

    // The old pipeline may still be referenced by a frame in flight, so it
    // dies with that frame rather than now.
    if (it != material_pipelines_.end() && it->second.pipeline != VK_NULL_HANDLE) {
        VkPipeline doomed = it->second.pipeline;
        if (resources_.deferred_deleter != nullptr) {
            resources_.deferred_deleter->defer(
                [device = device_, doomed] { vkDestroyPipeline(device, doomed, nullptr); });
        } else {
            vkDeviceWaitIdle(device_);
            vkDestroyPipeline(device_, doomed, nullptr);
        }
    }
    material_pipelines_[pipeline_id] = {pipeline, generation};
    FJELL_GFX_DEBUG("TerrainPass: built pipeline for '{}'",
                    pipeline_cache_->shader_path(pipeline_id));
    return pipeline;
}

void TerrainPass::resolve_node_material(const TerrainComponent& comp, VkPipeline& out_pipeline,
                                        uint32_t& out_material_id) {
    out_pipeline = VK_NULL_HANDLE;
    out_material_id = fallback_material_id_;
    if (comp.material != nullptr) {
        auto it = material_pipelines_.find(comp.material->pipeline_id());
        if (it != material_pipelines_.end() && it->second.pipeline != VK_NULL_HANDLE) {
            out_pipeline = it->second.pipeline;
            out_material_id = comp.material->material_id();
            return;
        }
    }
    if (fallback_ready_) {
        out_pipeline = material_pipelines_[fallback_pipeline_id_].pipeline;
    }
}

void TerrainPass::destroy_material_pipelines() {
    for (auto& [id, mp] : material_pipelines_) {
        if (mp.pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device_, mp.pipeline, nullptr);
        }
    }
    material_pipelines_.clear();
    fallback_ready_ = false;
}

void TerrainPass::destroy() {
    destroy_material_pipelines();
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
    if (task_module_ != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device_, task_module_, nullptr);
        task_module_ = VK_NULL_HANDLE;
    }
    if (mesh_module_ != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device_, mesh_module_, nullptr);
        mesh_module_ = VK_NULL_HANDLE;
    }
    if (descriptor_pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
        descriptor_pool_ = VK_NULL_HANDLE;
        terrain_set_ = VK_NULL_HANDLE;
    }
    if (terrain_set_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, terrain_set_layout_, nullptr);
        terrain_set_layout_ = VK_NULL_HANDLE;
    }
    bound_terrain_ = nullptr;
}

void TerrainPass::bind_terrain_textures(const TerrainAsset& terrain) {
    if (bound_terrain_ == &terrain) return;

    std::array<VkDescriptorImageInfo, 2> image_infos{};
    image_infos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    image_infos[0].imageView = terrain.height_view();
    image_infos[0].sampler = terrain.height_sampler();
    image_infos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    image_infos[1].imageView = terrain.weight_view();
    image_infos[1].sampler = terrain.weight_sampler();

    std::array<VkWriteDescriptorSet, 2> writes{};
    for (uint32_t i = 0; i < writes.size(); ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = terrain_set_;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &image_infos[i];
    }

    vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()), writes.data(), 0,
                           nullptr);
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

void TerrainPass::draw_terrain_nodes(const FrameContext& ctx, Variant variant,
                                     bool count_draws) {
    auto extent = ctx.target->extent();
    VkViewport vp{};
    vp.width = static_cast<float>(extent.width);
    vp.height = static_cast<float>(extent.height);
    vp.maxDepth = 1.0F;
    VkRect2D sc{};
    sc.extent = extent;

    VkPipeline bound_pipeline = VK_NULL_HANDLE;

    for (const auto& node : ctx.scene->objects()) {
        const auto* terrain_comp = node->components.get<TerrainComponent>();
        if (terrain_comp == nullptr || terrain_comp->terrain == nullptr) {
            continue;
        }
        if (variant == Variant::wireframe && !terrain_comp->show_wireframe) {
            continue;
        }

        const TerrainAsset& terrain = *terrain_comp->terrain;
        const uint32_t patches = terrain.patches_per_side();
        if (patches == 0) {
            continue;
        }

        // Only the colour variant reads the material; the others push the
        // same block with the fallback row, which they never look at.
        VkPipeline pipeline = VK_NULL_HANDLE;
        uint32_t material_id = fallback_material_id_;
        switch (variant) {
            case Variant::color:
                resolve_node_material(*terrain_comp, pipeline, material_id);
                break;
            case Variant::depth:
                pipeline = depth_pipeline_;
                break;
            case Variant::wireframe:
                pipeline = wireframe_pipeline_;
                break;
        }
        if (pipeline == VK_NULL_HANDLE) {
            continue;
        }

        bind_terrain_textures(terrain);

        if (pipeline != bound_pipeline) {
            vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            if (bound_pipeline == VK_NULL_HANDLE) {
                vkCmdSetViewport(ctx.cmd, 0, 1, &vp);
                vkCmdSetScissor(ctx.cmd, 0, 1, &sc);
            }
            bound_pipeline = pipeline;
        }

        std::array<VkDescriptorSet, 3> sets = {ctx.global_descriptor_set, ctx.bindless_set,
                                               terrain_set_};
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0,
                                static_cast<uint32_t>(sets.size()), sets.data(), 0,
                                nullptr);

        // The node's world translation positions the terrain, centred on the
        // pivot; the heightfield carries its own vertical placement on top.
        const auto* spatial = node->as_spatial();
        const glm::vec3 origin = terrain.world_min_corner(
            spatial != nullptr ? spatial->world_matrix_ : glm::mat4{1.0F});

        TerrainPushConstants push{};
        push.world_origin = {origin.x, origin.z};
        push.world_size = terrain.world_size();
        push.height_scale = terrain.height_scale();
        push.height_offset = terrain.height_offset() + origin.y;
        push.patches_per_side = patches;
        push.patch_quads = terrain.patch_quads();
        push.terrain_material_id = material_id;
        for (uint32_t i = 0; i < 4; ++i) {
            push.layer_material_ids[static_cast<int>(i)] = terrain.layer_material_id(i);
            push.layer_uv_scales[static_cast<int>(i)] = terrain.layer_uv_scale(i);
        }
        vkCmdPushConstants(ctx.cmd, layout_, TERRAIN_PUSH_STAGES, 0, sizeof(push), &push);

        draw_mesh_tasks_(ctx.cmd, patches * patches, 1, 1);

        if (count_draws && ctx.render_stats != nullptr) {
            ctx.render_stats->draw_calls++;
        }
    }
}

void TerrainPass::record(const FrameContext& ctx) {
    if (ctx.scene == nullptr || ctx.target == nullptr) {
        return;
    }

    FJELL_PROFILE_SCOPE_N("record_terrain");
    FJELL_GPU_ZONE(ctx, "Terrain");

    draw_terrain_nodes(ctx, Variant::color, true);

    // Patch grid overlay, drawn per node that asks for it. A second pass rather
    // than a branch inside the first: the lines sit on top of the shaded
    // surface, so they are a separate draw regardless.
    if (wireframe_pipeline_ != VK_NULL_HANDLE) {
        draw_terrain_nodes(ctx, Variant::wireframe, false);
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
    draw_terrain_nodes(ctx, Variant::depth, false);
}

} // namespace fjell
