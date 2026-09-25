#include "renderer/pass/water_pass.hpp"

#include "renderer/pass/water_fft_pass.hpp"
#include "renderer/pass_registry.hpp"

#include "core/base_path.hpp"
#include "core/log.hpp"
#include "renderer/bindless.hpp"
#include "renderer/components/water_component.hpp"
#include "renderer/gpu/deferred_deleter.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/gpu_context.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass/lit_surface_reads.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/render_stats.hpp"
#include "renderer/pass/pass_gpu_zone.hpp"
#include "renderer/pass/volumetric_fog_pass.hpp"
#include "renderer/resource_registry.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/resources/material.hpp"
#include "renderer/resources/shader_utils.hpp"
#include "renderer/resources/water_gpu.hpp"
#include "renderer/resources/water_surface.hpp"
#include "renderer/uniform_types.hpp"
#include "scene/scene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

namespace fjell {

namespace {

// Every stage declares the same push block, so every stage is named here.
constexpr VkShaderStageFlags WATER_PUSH_STAGES = VK_SHADER_STAGE_TASK_BIT_EXT |
                                                 VK_SHADER_STAGE_MESH_BIT_EXT |
                                                 VK_SHADER_STAGE_FRAGMENT_BIT;

// The engine's default water surface, drawn when a node has no material.
// Named for the engine rather than for water: a game is very likely to have
// its own shaders/water.fjsl, and a project shader takes precedence over the
// engine's at the same path — which is right, but would leave a water node
// with no material falling back to whatever the game happens to call water.
constexpr const char* FALLBACK_WATER_SHADER = "shaders/water_default.fjsl";

} // namespace

Result<> WaterPass::create(Device& device, const PassResources& resources) {
    device_ = device.handle();
    resources_ = resources;
    allocator_ = resources.allocator;
    draw_mesh_tasks_ = device.draw_mesh_tasks_fn();

    // The wave field lives on its own set: it changes with the water node,
    // not with the frame, and the global set has no free binding for a
    // per-node resource. Only the mesh stage reads it — the fragment stage
    // is handed what it needs through varyings rather than solving the field
    // a second time per pixel.
    std::array<VkDescriptorSetLayoutBinding, 7> set_bindings{};
    set_bindings[0].binding = 0;
    set_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    set_bindings[0].descriptorCount = 1;
    set_bindings[0].stageFlags = VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_COMPUTE_BIT;
    // The clipmap's level table. The task stage reads it to turn a workgroup
    // into a patch of a level; the mesh stage declares it so both stages see
    // the same set.
    set_bindings[1].binding = 1;
    set_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    set_bindings[1].descriptorCount = 1;
    set_bindings[1].stageFlags = VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT;
    // The simulated sea: its displacement now, its displacement a frame ago,
    // and its derivatives. A contributor that is an FFT's output has nothing
    // to put in the field buffer but where its textures tile.
    for (std::uint32_t binding = 2; binding <= 4; ++binding) {
        set_bindings[binding].binding = binding;
        set_bindings[binding].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        set_bindings[binding].descriptorCount = 1;
        set_bindings[binding].stageFlags = VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_COMPUTE_BIT;
    }
    // The wakes. The mesh stage moves the surface by the wide ones; the
    // fragment stage shades the narrow ones per pixel and reads how churned
    // the water is, which is the one thing here it solves for itself.
    set_bindings[5].binding = 5;
    set_bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    set_bindings[5].descriptorCount = 1;
    set_bindings[5].stageFlags = VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_FRAGMENT_BIT
                               | VK_SHADER_STAGE_COMPUTE_BIT;
    // The fog integrated front to back, which the fragment stage samples at
    // the surface and at the bed for the medium between them.
    set_bindings[6].binding = 6;
    set_bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    set_bindings[6].descriptorCount = 1;
    set_bindings[6].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo set_layout_info{};
    set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout_info.bindingCount = static_cast<uint32_t>(set_bindings.size());
    set_layout_info.pBindings = set_bindings.data();
    if (vkCreateDescriptorSetLayout(device_, &set_layout_info, nullptr,
                                    &water_set_layout_) != VK_SUCCESS) {
        return make_error("Failed to create water set layout");
    }

    // Each node's field is bound at its own offset into the frame's buffer,
    // so the records sit at the alignment the device asks of a uniform
    // buffer offset.
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(device.physical_device(), &props);
    const VkDeviceSize alignment = std::max<VkDeviceSize>(
        props.limits.minUniformBufferOffsetAlignment, 1);
    field_stride_ = (sizeof(GpuWaterField) + alignment - 1) / alignment * alignment;

    for (auto& buffer : field_buffers_) {
        buffer = std::make_unique<Buffer>(
            allocator_, field_stride_ * WATER_NODES_PER_FRAME,
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT);
        // A field of flat water in the first record, so the waterline
        // dispatch that runs with no water in reach never binds
        // uninitialised memory.
        GpuWaterField empty{};
        buffer->write(&empty, sizeof(empty));
    }
    for (auto& buffer : clipmap_buffers_) {
        buffer = std::make_unique<Buffer>(
            allocator_, sizeof(GpuWaterClipmap), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT);
        const GpuWaterClipmap table = clipmap_.gpu();
        buffer->write(&table, sizeof(table));
    }

    for (auto& buffer : wake_buffers_) {
        buffer = std::make_unique<Buffer>(
            allocator_, WAKE_RECORDS_PER_FRAME * sizeof(GpuWakeSource),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT);
        buffer->map();
    }

    auto task_code = read_spirv(base_path::resolve("shaders/water.task.spv"));
    auto mesh_code = read_spirv(base_path::resolve("shaders/water.mesh.spv"));

    // Task and mesh modules outlive create(): the colour pipelines are built
    // per water material, whenever one is first seen or recompiles.
    task_module_ = create_shader_module(device_, task_code);
    mesh_module_ = create_shader_module(device_, mesh_code);

    // Set 1 is the material set — bindless textures, material rows, params —
    // that the FJSL fragment reads its own uniforms from.
    std::array<VkDescriptorSetLayout, 3> set_layouts = {resources.global_set_layout,
                                                        resources.material_set_layout,
                                                        water_set_layout_};

    VkPushConstantRange push_range{};
    push_range.stageFlags = WATER_PUSH_STAGES;
    push_range.offset = 0;
    push_range.size = sizeof(WaterPushConstants);

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = static_cast<uint32_t>(set_layouts.size());
    layout_info.pSetLayouts = set_layouts.data();
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_range;

    if (vkCreatePipelineLayout(device_, &layout_info, nullptr, &layout_) != VK_SUCCESS) {
        return make_error("Failed to create water pipeline layout");
    }

    // The prepass: the surface's depth and nothing else. The same task and
    // mesh stages as every colour pipeline, so the depth is the displaced
    // surface to the bit; no fragment stage, since nothing it would compute
    // is kept. Tests and writes LESS against the seeded scene depth.
    {
        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_TASK_BIT_EXT;
        stages[0].module = task_module_;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_MESH_BIT_EXT;
        stages[1].module = mesh_module_;
        stages[1].pName = "main";

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
        rasterizer.cullMode = VK_CULL_MODE_NONE;   // as the colour pipelines: seen from both sides
        rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.rasterizationSamples = resources.sample_count;

        VkPipelineDepthStencilStateCreateInfo depth_stencil{};
        depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth_stencil.depthTestEnable = VK_TRUE;
        depth_stencil.depthWriteEnable = VK_TRUE;
        depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS;

        VkPipelineRenderingCreateInfo rendering_info{};
        rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        rendering_info.depthAttachmentFormat = resources.depth_format;

        VkGraphicsPipelineCreateInfo pipeline_ci{};
        pipeline_ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeline_ci.pNext = &rendering_info;
        pipeline_ci.stageCount = static_cast<uint32_t>(stages.size());
        pipeline_ci.pStages = stages.data();
        pipeline_ci.pViewportState = &viewport_state;
        pipeline_ci.pRasterizationState = &rasterizer;
        pipeline_ci.pMultisampleState = &multisampling;
        pipeline_ci.pDepthStencilState = &depth_stencil;
        pipeline_ci.pDynamicState = &dynamic_state;
        pipeline_ci.layout = layout_;
        if (vkCreateGraphicsPipelines(device_, resources.pipeline_cache, 1, &pipeline_ci, nullptr,
                                      &depth_pipeline_) != VK_SUCCESS) {
            return make_error("Failed to create water depth prepass pipeline");
        }
    }

    // The waterline: a compute dispatch over the froxel columns that asks the
    // wave field, for each, whether the near plane's point lies on the other
    // side of the surface from the camera. Its set 2 is the water set the mesh
    // stage reads; set 3 is the mask it writes.
    {
        VkDescriptorSetLayoutBinding mask_binding{};
        mask_binding.binding = 0;
        mask_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        mask_binding.descriptorCount = 1;
        mask_binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo mask_layout_info{};
        mask_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        mask_layout_info.bindingCount = 1;
        mask_layout_info.pBindings = &mask_binding;
        if (vkCreateDescriptorSetLayout(device_, &mask_layout_info, nullptr, &line_set_layout_)
            != VK_SUCCESS) {
            return make_error("Failed to create waterline set layout");
        }

        std::array<VkDescriptorSetLayout, 4> line_sets = {resources.global_set_layout,
                                                          resources.material_set_layout,
                                                          water_set_layout_, line_set_layout_};
        VkPushConstantRange line_push{};
        line_push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        line_push.size = sizeof(WaterPushConstants);
        VkPipelineLayoutCreateInfo line_layout_info{};
        line_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        line_layout_info.setLayoutCount = static_cast<uint32_t>(line_sets.size());
        line_layout_info.pSetLayouts = line_sets.data();
        line_layout_info.pushConstantRangeCount = 1;
        line_layout_info.pPushConstantRanges = &line_push;
        if (vkCreatePipelineLayout(device_, &line_layout_info, nullptr, &line_layout_)
            != VK_SUCCESS) {
            return make_error("Failed to create waterline pipeline layout");
        }

        auto line_code = read_spirv(base_path::resolve("shaders/water_line.comp.spv"));
        line_module_ = create_shader_module(device_, line_code);
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = line_module_;
        stage.pName = "main";
        VkComputePipelineCreateInfo line_ci{};
        line_ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        line_ci.stage = stage;
        line_ci.layout = line_layout_;
        if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &line_ci, nullptr,
                                     &line_pipeline_) != VK_SUCCESS) {
            return make_error("Failed to create waterline pipeline");
        }
    }

    return {};
}

void WaterPass::destroy() {
    destroy_material_pipelines();
    if (depth_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, depth_pipeline_, nullptr);
        depth_pipeline_ = VK_NULL_HANDLE;
    }
    for (auto& buffer : field_buffers_) buffer.reset();
    for (auto& buffer : clipmap_buffers_) buffer.reset();
    for (auto& buffer : wake_buffers_) buffer.reset();
    if (line_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, line_pipeline_, nullptr);
        line_pipeline_ = VK_NULL_HANDLE;
    }
    if (line_layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, line_layout_, nullptr);
        line_layout_ = VK_NULL_HANDLE;
    }
    if (line_set_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, line_set_layout_, nullptr);
        line_set_layout_ = VK_NULL_HANDLE;
    }
    if (line_module_ != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device_, line_module_, nullptr);
        line_module_ = VK_NULL_HANDLE;
    }
    if (mesh_module_ != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device_, mesh_module_, nullptr);
        mesh_module_ = VK_NULL_HANDLE;
    }
    if (task_module_ != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device_, task_module_, nullptr);
        task_module_ = VK_NULL_HANDLE;
    }
    if (layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, layout_, nullptr);
        layout_ = VK_NULL_HANDLE;
    }
    if (water_set_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, water_set_layout_, nullptr);
        water_set_layout_ = VK_NULL_HANDLE;
    }
    field_slots_.clear();
}

void WaterPass::destroy_material_pipelines() {
    for (auto& [id, mp] : material_pipelines_) {
        if (mp.pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device_, mp.pipeline, nullptr);
        }
    }
    material_pipelines_.clear();
}

void WaterPass::post_create(GpuContext& gpu, PassRegistry& passes) {
    pipeline_cache_ = gpu.pipeline_cache();
    bindless_ = &gpu.bindless_textures();
    simulation_ = passes.try_get<WaterFftPass>("water_fft");
    fog_ = passes.try_get<FogCore>("fog_medium");
    fallback_volume_ = gpu.volume_fallback_view();
    if (simulation_ == nullptr) {
        FJELL_CORE_ERROR("WaterPass: no water_fft pass in the render graph. Water samples the "
                         "simulated sea's textures whether or not a field has one, so without "
                         "that pass it cannot draw.");
    }
}

void WaterPass::prepare_frame(const FrameContext& ctx, GpuContext& /*gpu*/) {
    depth_drawn_for_ = nullptr;
    if (ctx.scene == nullptr || pipeline_cache_ == nullptr) return;

    // The levels follow the camera, not any particular body of water, so
    // they are recentered once for the frame regardless of how many infinite
    // seas the scene holds.
    clipmap_.recenter(ctx.camera_position);

    // How far the camera stands above each body of water, negative under
    // it. Asked of the surface over the camera's column, since the trains
    // move the surface sideways and the height at the camera's rest position
    // is not the height above it.
    camera_gap_.clear();
    for (const auto& node : ctx.scene->objects()) {
        const auto* wc = node->components.get<WaterComponent>();
        if (wc == nullptr) continue;
        const auto* spatial = node->as_spatial();
        if (spatial == nullptr) continue;
        const glm::vec3 centre{spatial->world_matrix_[3]};
        const glm::vec2 cam_xz(ctx.camera_position.x, ctx.camera_position.z);
        if (wc->extent == WaterComponent::Extent::bounded) {
            const float half = wc->size * 0.5F;
            if (std::abs(cam_xz.x - centre.x) > half || std::abs(cam_xz.y - centre.z) > half) {
                continue;
            }
        }
        const float surface = centre.y
            + water::height_at_iterative(wc->effective_water, cam_xz, ctx.elapsed_time);
        camera_gap_[node->id] = ctx.camera_position.y - surface;
    }

    // The world runs one simulated sea: the first a water node asks for.
    if (simulation_ != nullptr) {
        const FjWaterData::SimulatedSea* running = nullptr;
        std::optional<FftContributor> sea;
        const Node* asked_first = nullptr;
        const Node* asked_otherwise = nullptr;
        for (const auto& node : ctx.scene->objects()) {
            const auto* wc = node->components.get<WaterComponent>();
            if (wc == nullptr) continue;
            const auto* fft = water::simulated_sea(wc->effective_water);
            if (fft == nullptr) continue;
            if (!sea.has_value()) {
                sea = *fft;
                asked_first = node.get();
                // Drawn by whoever derived this field. A field nothing has
                // derived yet has no sea to show, and gets one next frame.
                if (wc->effective_water.simulated.has_value()) {
                    running = &*wc->effective_water.simulated;
                }
            } else if (*fft != *sea && asked_otherwise == nullptr) {
                asked_otherwise = node.get();
            }
        }
        // A second body of water with a simulated sea of its own is drawn with
        // the first one's: same textures, at the scale they were made for. Said
        // once, when it starts being true, rather than left to be puzzled over.
        if (asked_otherwise != nullptr && !warned_second_sea_) {
            FJELL_CORE_WARN("Water: '{}' asks for a different simulated sea than '{}'. The world "
                            "runs one simulation, so both are drawn with the sea '{}' describes.",
                            asked_otherwise->name, asked_first->name, asked_first->name);
        }
        warned_second_sea_ = asked_otherwise != nullptr;
        simulation_->set_sea(running);
    }

    upload_fields(ctx);
    upload_clipmap(ctx);
    upload_wakes(ctx);

    // Pipelines are built here rather than in record(): a first sighting of a
    // material or a hot-reloaded shader means vkCreateGraphicsPipelines,
    // which does not belong inside command recording.
    bool any = false;
    for (const auto& node : ctx.scene->objects()) {
        const auto* wc = node->components.get<WaterComponent>();
        if (wc == nullptr) continue;
        any = true;
        if (wc->material != nullptr) {
            (void)ensure_material_pipeline(wc->material->pipeline_id());
        }
    }
    if (!any) return;
    ensure_fallback_material();
    // A hot-reloaded water.fjsl moves the generation of the fallback entry
    // like any other; keep its pipeline current too.
    if (fallback_ready_) {
        (void)ensure_material_pipeline(fallback_pipeline_id_);
    }
}

void WaterPass::ensure_fallback_material() {
    if (fallback_ready_ || pipeline_cache_ == nullptr || bindless_ == nullptr) return;

    const uint32_t pid = pipeline_cache_->register_shader(FALLBACK_WATER_SHADER);
    if (pipeline_cache_->has_compile_error(pid)) {
        FJELL_GFX_ERROR("WaterPass: '{}' failed to compile: {}", FALLBACK_WATER_SHADER,
                        pipeline_cache_->compile_error(pid));
        return;
    }
    const auto& meta = pipeline_cache_->metadata(pid);

    // A material row seeded with the shader's own defaults, as TerrainPass
    // does for a terrain with no .fjmat behind it.
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

VkPipeline WaterPass::ensure_material_pipeline(uint32_t pipeline_id) {
    if (pipeline_cache_ == nullptr || pipeline_id >= pipeline_cache_->shader_count()) {
        return VK_NULL_HANDLE;
    }
    const uint32_t generation = pipeline_cache_->generation(pipeline_id);
    auto it = material_pipelines_.find(pipeline_id);
    if (it != material_pipelines_.end() && it->second.generation == generation) {
        return it->second.pipeline;
    }

    const auto& meta = pipeline_cache_->metadata(pipeline_id);
    if (meta.shader_type != ShaderType::Water) {
        if (reported_entries_.insert(pipeline_id).second) {
            FJELL_GFX_ERROR("WaterPass: '{}' is shader_type '{}', water needs "
                            "shader_type water",
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
    // A sea is seen from under it as well as over it, and a wave steep enough
    // to fold shows its own back face. Neither is a reason to drop a triangle.
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = resources_.sample_count;

    // Water blends over the scene behind it. The three data attachments take
    // the fragment's values straight: a velocity or normal averaged with what
    // the opaque pass left there belongs to neither surface.
    VkPipelineColorBlendAttachmentState color_blend{};
    color_blend.blendEnable = VK_TRUE;
    color_blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    color_blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    color_blend.colorBlendOp = VK_BLEND_OP_ADD;
    color_blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    color_blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    color_blend.alphaBlendOp = VK_BLEND_OP_ADD;
    color_blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                 VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendAttachmentState data_blend{};
    data_blend.blendEnable = VK_FALSE;
    data_blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    std::array blend_attachments = {color_blend, data_blend, data_blend, data_blend};
    VkPipelineColorBlendStateCreateInfo color_blending{};
    color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blending.attachmentCount = static_cast<uint32_t>(blend_attachments.size());
    color_blending.pAttachments = blend_attachments.data();

    // Tests against the depth the prepass wrote, and writes none: only the
    // nearest surface at each pixel passes, so the sea is hidden by what
    // stands in front of it and hides itself.
    //
    // A Gerstner surface displaces horizontally, and where the trains pinch
    // past their limit it folds through itself: a crest then genuinely stands
    // in front of the water behind it, and without the depth the sheets would
    // composite in submission order, which reads as geometry going missing at
    // the crests. Two bodies of water overlapping on screen resolve the same
    // way: the nearer one shades.
    //
    // The attachment is the water's own — `water_depth`, seeded from the scene
    // depth — and not the depth the rest of the frame samples. That separation
    // is the whole design: a dozen shaders read `depth_texture` meaning "the
    // opaque scene", and a transparent surface does not belong in it.
    //
    // LESS_OR_EQUAL because the prepass wrote this surface's own depth, which
    // LESS would reject; the mesh stage's invariant position makes the two
    // draws agree to the bit.
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

    VkGraphicsPipelineCreateInfo pipeline_ci{};
    pipeline_ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_ci.pNext = &rendering_info;
    pipeline_ci.stageCount = static_cast<uint32_t>(stages.size());
    pipeline_ci.pStages = stages.data();
    pipeline_ci.pViewportState = &viewport_state;
    pipeline_ci.pRasterizationState = &rasterizer;
    pipeline_ci.pMultisampleState = &multisampling;
    pipeline_ci.pDepthStencilState = &depth_stencil;
    pipeline_ci.pColorBlendState = &color_blending;
    pipeline_ci.pDynamicState = &dynamic_state;
    pipeline_ci.layout = layout_;
    pipeline_ci.renderPass = VK_NULL_HANDLE;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult result = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline_ci,
                                                nullptr, &pipeline);
    vkDestroyShaderModule(device_, frag_module, nullptr);

    if (result != VK_SUCCESS) {
        FJELL_GFX_ERROR("WaterPass: failed to create pipeline for '{}'",
                        pipeline_cache_->shader_path(pipeline_id));
        return it != material_pipelines_.end() ? it->second.pipeline : VK_NULL_HANDLE;
    }

    if (it != material_pipelines_.end() && it->second.pipeline != VK_NULL_HANDLE) {
        // Retired rather than destroyed: a hot reload idles the device first,
        // but a material first seen while the scene is already drawing does
        // not, and the frame slot's own fence only proves that slot is done —
        // another slot's command buffer can still reference this pipeline.
        // The deleter holds it until every slot has cycled.
        VkPipeline doomed = it->second.pipeline;
        if (resources_.deferred_deleter != nullptr) {
            resources_.deferred_deleter->defer(
                [device = device_, doomed] { vkDestroyPipeline(device, doomed, nullptr); });
        } else {
            vkDeviceWaitIdle(device_);
            vkDestroyPipeline(device_, doomed, nullptr);
        }
    }
    material_pipelines_[pipeline_id] = MaterialPipeline{pipeline, generation};
    return pipeline;
}

void WaterPass::resolve_node_material(const WaterComponent& comp, VkPipeline& out_pipeline,
                                       uint32_t& out_material_id) {
    out_pipeline = VK_NULL_HANDLE;
    out_material_id = fallback_material_id_;

    if (comp.material != nullptr) {
        // The pipeline comes from the shared material either way: a script's
        // override clones the parent's params into its own SSBO row, it does
        // not change which shader draws the surface.
        VkPipeline p = ensure_material_pipeline(comp.material->pipeline_id());
        if (p != VK_NULL_HANDLE) {
            out_pipeline = p;
            out_material_id = comp.resolved_material_id();
            return;
        }
    }
    // No material, or one that is not usable water: the engine's own shader
    // at its defaults, so a node still draws rather than vanishing.
    if (fallback_ready_) {
        auto it = material_pipelines_.find(fallback_pipeline_id_);
        if (it != material_pipelines_.end()) {
            out_pipeline = it->second.pipeline;
        }
    }
}

void WaterPass::upload_fields(const FrameContext& ctx) {
    field_slots_.clear();
    const std::uint32_t slot = ctx.frame_index % MAX_FRAMES_IN_FLIGHT;
    auto* records = static_cast<std::byte*>(field_buffers_[slot]->mapped());
    std::uint32_t used = 0;
    for (const auto& node : ctx.scene->objects()) {
        const auto* wc = node->components.get<WaterComponent>();
        if (wc == nullptr) continue;
        if (used >= WATER_NODES_PER_FRAME) break;
        GpuWaterField field = to_gpu_field(wc->effective_water);
        // A field with a simulated contributor is drawn with the sea the
        // world is running, which is its own unless another body of water
        // asked first. The textures tile at the running sea's patch whoever
        // samples them, so that is the scale the shader is given — not the
        // one this field would have had. None running yet means none to
        // sample.
        if (field.simulated_patch_size > 0.0f) {
            field.simulated_patch_size = simulation_->active() ? simulation_->patch_size() : 0.0f;
        }
        const VkDeviceSize offset = used * field_stride_;
        std::memcpy(records + offset, &field, sizeof(field));
        field_slots_[wc] = FieldSlot{offset, field.simulated_patch_size};
        ++used;
    }
}

void WaterPass::record_water_line(const FrameContext& ctx, VkImageView mask, VkExtent2D size) {
    if (ctx.scene == nullptr || line_pipeline_ == VK_NULL_HANDLE || simulation_ == nullptr) return;

    // The body of water nearest the camera, if any is within reach of the
    // near plane. One: a camera cannot be at two surfaces at once.
    const Node* nearest = nullptr;
    const WaterComponent* water = nullptr;
    float nearest_gap = LINE_REACH_METRES;
    for (const auto& node : ctx.scene->objects()) {
        const auto* wc = node->components.get<WaterComponent>();
        if (wc == nullptr) continue;
        const auto gap = camera_gap_.find(node->id);
        if (gap == camera_gap_.end() || std::abs(gap->second) >= nearest_gap) continue;
        nearest_gap = std::abs(gap->second);
        nearest = node.get();
        water = wc;
    }

    WaterPushConstants push{};
    VkDeviceSize field_offset = 0;
    const auto field = water != nullptr ? field_slots_.find(water) : field_slots_.end();
    if (field != field_slots_.end()) {
        field_offset = field->second.offset;
        const auto* spatial = nearest->as_spatial();
        push.surface_height = spatial != nullptr ? spatial->world_matrix_[3].y : 0.0F;
        push.simulated_patch_size = field->second.simulated_patch_size;
        if (const auto range = wake_ranges_.find(water); range != wake_ranges_.end()) {
            push.wake_first = range->second.first;
            push.wake_count = range->second.count;
        }
        push.camera_below = camera_below(nearest->id) ? 1U : 0U;
        push.line_active = 1;
    }

    FrameCacheBinding mask_binding{};
    mask_binding.binding = 0;
    mask_binding.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    mask_binding.image.imageView = mask;
    mask_binding.image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    const VkDescriptorSet mask_set =
        ctx.descriptor_cache->acquire(line_set_layout_, std::span{&mask_binding, 1});

    std::array<VkDescriptorSet, 4> sets = {ctx.global_descriptor_set, ctx.bindless_set,
                                          acquire_water_set(ctx, field_offset, VK_NULL_HANDLE),
                                          mask_set};
    vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, line_pipeline_);
    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, line_layout_, 0,
                            static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
    vkCmdPushConstants(ctx.cmd, line_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(ctx.cmd, (size.width + 7) / 8, (size.height + 7) / 8, 1);
}

void WaterPass::upload_wakes(const FrameContext& ctx) {
    wake_ranges_.clear();
    const std::uint32_t slot = ctx.frame_index % MAX_FRAMES_IN_FLIGHT;
    auto* records = static_cast<GpuWakeSource*>(wake_buffers_[slot]->mapped());
    std::uint32_t used = 0;
    for (const auto& node : ctx.scene->objects()) {
        const auto* wc = node->components.get<WaterComponent>();
        if (wc == nullptr) continue;
        WakeRange range{used, 0};
        for (const water::WakeSource& wake : wc->effective_water.wakes) {
            if (used >= WAKE_RECORDS_PER_FRAME) break;
            records[used++] = to_gpu_wake(wake);
            ++range.count;
        }
        wake_ranges_[wc] = range;
    }
}

VkDescriptorSet WaterPass::acquire_water_set(const FrameContext& ctx,
                                             VkDeviceSize field_offset,
                                             VkImageView medium) const {
    auto sampled = [this](std::uint32_t binding, VkImageView view) {
        FrameCacheBinding b{};
        b.binding = binding;
        b.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.image.sampler = simulation_->sampler();
        b.image.imageView = view;
        b.image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        return b;
    };
    auto uniform = [](std::uint32_t binding, VkBuffer buffer, VkDeviceSize offset,
                      VkDeviceSize range) {
        FrameCacheBinding b{};
        b.binding = binding;
        b.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b.buffer.buffer = buffer;
        b.buffer.offset = offset;
        b.buffer.range = range;
        return b;
    };
    auto storage = [](std::uint32_t binding, VkBuffer buffer, VkDeviceSize range) {
        FrameCacheBinding b{};
        b.binding = binding;
        b.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b.buffer.buffer = buffer;
        b.buffer.range = range;
        return b;
    };
    const std::uint32_t slot = ctx.frame_index % MAX_FRAMES_IN_FLIGHT;
    FrameCacheBinding volume{};
    volume.binding = 6;
    volume.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    volume.image.sampler = fog_ != nullptr ? fog_->fog_sampler() : simulation_->sampler();
    volume.image.imageView = medium != VK_NULL_HANDLE ? medium : fallback_volume_;
    volume.image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const FrameCacheBinding bindings[7]{
        uniform(0, field_buffers_[slot]->handle(), field_offset, sizeof(GpuWaterField)),
        uniform(1, clipmap_buffers_[slot]->handle(), 0, sizeof(GpuWaterClipmap)),
        sampled(2, simulation_->displacement_view()),
        sampled(3, simulation_->previous_displacement_view()),
        sampled(4, simulation_->derivatives_view()),
        storage(5, wake_buffers_[slot]->handle(), WAKE_RECORDS_PER_FRAME * sizeof(GpuWakeSource)),
        volume,
    };
    return ctx.descriptor_cache->acquire(water_set_layout_, {bindings, 7});
}

void WaterPass::upload_clipmap(const FrameContext& ctx) {
    const std::uint32_t slot = ctx.frame_index % MAX_FRAMES_IN_FLIGHT;
    const GpuWaterClipmap& table = clipmap_.gpu();
    std::memcpy(clipmap_buffers_[slot]->mapped(), &table, sizeof(table));
}

bool WaterPass::scene_has_water(const FrameContext& ctx) {
    if (ctx.scene == nullptr) {
        return false;
    }
    for (const auto& node : ctx.scene->objects()) {
        if (node->components.get<WaterComponent>() != nullptr) {
            return true;
        }
    }
    return false;
}

bool WaterPass::draws(const FrameContext& ctx) const {
    return ctx.target != nullptr && draw_mesh_tasks_ != nullptr && simulation_ != nullptr &&
           scene_has_water(ctx);
}

void WaterPass::seed_water_depth(const FrameContext& ctx) {
    if (!draws(ctx)) return;
    const VkImage scene_depth = ctx.target->msaa_depth_image();
    const VkImage water_depth = ctx.target->water_depth_image();
    if (scene_depth == VK_NULL_HANDLE || water_depth == VK_NULL_HANDLE) return;

    FJELL_PROFILE_SCOPE_N("record_water_depth_seed");
    FJELL_GPU_ZONE(ctx, "Water Depth Seed");

    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    region.dstSubresource = region.srcSubresource;
    region.extent = {ctx.target->extent().width, ctx.target->extent().height, 1};
    vkCmdCopyImage(ctx.cmd, scene_depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   water_depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

PassSetup WaterPass::setup() const {
    return {
        .name = "water",
        .phase = RenderPhase::Color,
        .sub_order = 45,   // before the transparent pass (50), which carries on over it
    };
}

void WaterPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    if (!ctx.water_draws) { return; }
    // The colour targets the transparent pass then loads, drawn over.
    for (const char* name : {"msaa_color", "msaa_mr", "msaa_normal", "msaa_velocity"}) {
        builder.write(builder.import_named(ctx, name), ResourceAccess::color_attachment);
    }
    // The depth the prepass wrote, tested and not written.
    builder.read(builder.import_named(ctx, "msaa_surface_depth"),
                 ResourceAccess::depth_attachment_read);
    // What the surface shader samples through the global set: the lighting,
    // the scene depth behind it for WATER_DEPTH, and the copy of the scene
    // it refracts.
    declare_lit_surface_reads(builder, ctx);
    builder.read(builder.import_named(ctx, "depth"), ResourceAccess::sampled_fragment);
    builder.read(builder.import_named(ctx, "screen_color"), ResourceAccess::sampled_fragment);
    // The simulated sea, which the mesh stage moves the surface by.
    declare_simulation_reads(builder, ctx, ResourceAccess::sampled_mesh);
    // The fog integrated front to back, for the medium behind the surface.
    // A transient: read by re-creating it by name, which is also what keeps
    // it from being aliased before this reads it.
    if (ctx.volumetric_fog_enabled) {
        builder.read(builder.create(FogCore::INTEGRATED_NAME, FogCore::volume_desc()),
                     ResourceAccess::sampled_fragment);
    }
}

void WaterPass::declare_simulation_reads(PassBuilder& builder, const DeclareContext& ctx,
                                         ResourceAccess access) {
    for (const char* name : {"water_displacement", "water_displacement_prev",
                             "water_derivatives"}) {
        if (auto image = builder.import_named_optional(ctx, name); image.valid()) {
            builder.read(image, access);
        }
    }
}

void WaterPass::draw_nodes(const FrameContext& ctx, VkPipeline depth_only, VkImageView medium) {
    // Its own scope means its own viewport and scissor.
    VkViewport vp{};
    vp.width = static_cast<float>(ctx.target->extent().width);
    vp.height = static_cast<float>(ctx.target->extent().height);
    vp.maxDepth = 1.0F;
    vkCmdSetViewport(ctx.cmd, 0, 1, &vp);

    VkRect2D sc{};
    sc.extent = ctx.target->extent();
    vkCmdSetScissor(ctx.cmd, 0, 1, &sc);

    VkPipeline current = VK_NULL_HANDLE;

    for (const auto& node : ctx.scene->objects()) {
        const auto* wc = node->components.get<WaterComponent>();
        if (wc == nullptr) continue;
        // Absent when the frame's field buffer was full.
        const auto field = field_slots_.find(wc);
        if (field == field_slots_.end()) continue;

        // A node without a usable colour pipeline is not drawn at all, its
        // depth included, so the surface depth never holds water nobody sees.
        VkPipeline pipeline = VK_NULL_HANDLE;
        uint32_t material_id = 0;
        resolve_node_material(*wc, pipeline, material_id);
        if (pipeline == VK_NULL_HANDLE) continue;
        if (depth_only != VK_NULL_HANDLE) pipeline = depth_only;

        // Each node reads its own field record, so the set is per node.
        std::array<VkDescriptorSet, 3> sets = {
            ctx.global_descriptor_set, ctx.bindless_set,
            acquire_water_set(ctx, field->second.offset, medium)};
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0,
                                static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
        if (pipeline != current) {
            vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            current = pipeline;
        }

        const bool infinite = wc->extent == WaterComponent::Extent::infinite;
        const uint32_t patches = wc->patches_per_side > 0 ? wc->patches_per_side : 1;
        const auto* spatial = node->as_spatial();
        const glm::mat4 world = spatial != nullptr ? spatial->world_matrix_ : glm::mat4{1.0F};
        const glm::vec3 centre = glm::vec3(world[3]);

        WaterPushConstants push{};
        // The node's position is the middle of the surface, which is where a
        // placed thing is expected to be; the shaders generate from a corner.
        // An infinite sea takes only the height from the node — it has no
        // corner and no size, and the clipmap says where its patches are.
        push.world_origin = glm::vec2(centre.x, centre.z) - glm::vec2(wc->size * 0.5F);
        push.world_size = wc->size;
        push.surface_height = centre.y;
        push.patches_per_side = patches;
        push.patch_quads = static_cast<uint32_t>(WATER_PATCH_QUADS);
        push.water_material_id = material_id;
        push.delta_time = ctx.delta_time;
        push.wave_direction = water::dominant_direction(wc->effective_water);
        push.infinite = infinite ? 1U : 0U;
        push.simulated_patch_size = field->second.simulated_patch_size;
        push.sdf_volume_min = sdf_volume_min_;
        push.sdf_volume_max = sdf_volume_max_;
        if (const auto range = wake_ranges_.find(wc); range != wake_ranges_.end()) {
            push.wake_first = range->second.first;
            push.wake_count = range->second.count;
        }
        push.camera_below = camera_below(node->id) ? 1U : 0U;
        // The froxel range to map the medium's samples by, for a water that
        // names a medium, in a run that built the fog volume.
        if (medium != VK_NULL_HANDLE && fog_ != nullptr && wc->medium_material != nullptr) {
            push.medium_near = FogCore::NEAR_PLANE;
            push.medium_far = fog_->far_plane();
            push.medium_slices = FogCore::DEPTH_SLICES;
        }

        vkCmdPushConstants(ctx.cmd, layout_, WATER_PUSH_STAGES, 0, sizeof(push), &push);
        draw_mesh_tasks_(ctx.cmd, infinite ? WaterClipmap::patch_count() : patches * patches,
                         1, 1);

        if (ctx.render_stats != nullptr) {
            ctx.render_stats->draw_calls++;
        }
    }
}

void WaterPass::record_depth(const FrameContext& ctx) {
    if (!draws(ctx) || depth_pipeline_ == VK_NULL_HANDLE) return;

    FJELL_PROFILE_SCOPE_N("record_water_depth");
    FJELL_GPU_ZONE(ctx, "Water Depth");

    auto* target = ctx.target;

    // Loaded, because the seed filled it with the scene depth that hides the
    // water behind the opaque scene; stored, because the colour draw tests
    // against it. Under MSAA it also resolves into the surface depth the
    // rest of the frame samples; at one sample it is that image.
    VkRenderingAttachmentInfo depth_attachment{};
    depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth_attachment.imageView = target->water_depth_view();
    depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    if (target->sample_count() != VK_SAMPLE_COUNT_1_BIT) {
        depth_attachment.resolveImageView = target->resolve_water_depth_view();
        depth_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth_attachment.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
    }

    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.extent = target->extent();
    ri.layerCount = 1;
    ri.pDepthAttachment = &depth_attachment;
    vkCmdBeginRendering(ctx.cmd, &ri);
    draw_nodes(ctx, depth_pipeline_, VK_NULL_HANDLE);
    vkCmdEndRendering(ctx.cmd);

    depth_drawn_for_ = target;
}

void WaterPass::record(const FrameContext& ctx) {
    if (!draws(ctx)) return;
    // Without the prepass the attachment holds only the seeded scene, and
    // the water would draw untested against itself; a graph that lists the
    // water without it shows none.
    if (depth_drawn_for_ != ctx.target) {
        if (!warned_no_prepass_) {
            FJELL_GFX_WARN("WaterPass: water_depth_prepass did not draw before the water, so "
                           "the water is skipped. A render graph with the water node needs "
                           "water_depth_seed and water_depth_prepass too.");
            warned_no_prepass_ = true;
        }
        return;
    }
    depth_drawn_for_ = nullptr;

    FJELL_PROFILE_SCOPE_N("record_water");
    FJELL_GPU_ZONE(ctx, "Water");

    auto* target = ctx.target;

    // The sea draws with a depth attachment of its own, because the
    // transparent pass binds the scene depth — a dozen shaders sample that
    // image meaning "the opaque scene", which the water is not part of. Same
    // colour targets, loaded and stored, so the transparent pass that follows
    // carries on over the top of the water.
    //
    // No colour resolve here: the transparent pass resolves the same
    // attachments at its end, and that resolve sees the water's contribution
    // because it resolves the whole image. Resolving twice would be correct
    // and wasted.
    std::array<VkRenderingAttachmentInfo, 4> color_attachments{};
    const std::array<VkImageView, 4> views = {
        target->msaa_color_view(), target->msaa_mr_view(),
        target->msaa_normal_view(), target->msaa_velocity_view()};
    for (std::size_t i = 0; i < color_attachments.size(); ++i) {
        color_attachments[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color_attachments[i].imageView = views[i];
        color_attachments[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_attachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        color_attachments[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color_attachments[i].resolveMode = VK_RESOLVE_MODE_NONE;
    }

    // The depth the prepass left: tested, not written, and kept as it is,
    // since it already is the surface depth.
    VkRenderingAttachmentInfo depth_attachment{};
    depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth_attachment.imageView = target->water_depth_view();
    depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_NONE;

    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.offset = {0, 0};
    ri.renderArea.extent = target->extent();
    ri.layerCount = 1;
    ri.colorAttachmentCount = static_cast<uint32_t>(color_attachments.size());
    ri.pColorAttachments = color_attachments.data();
    ri.pDepthAttachment = &depth_attachment;
    // The fog integrated front to back, when this run built it.
    VkImageView medium = VK_NULL_HANDLE;
    if (ctx.registry != nullptr) {
        if (const auto* volume = ctx.registry->find_image(FogCore::INTEGRATED_NAME)) {
            medium = volume->view;
        }
    }

    vkCmdBeginRendering(ctx.cmd, &ri);
    draw_nodes(ctx, VK_NULL_HANDLE, medium);
    vkCmdEndRendering(ctx.cmd);
}

// ── WaterDepthSeedPass ──────────────────────────────────────────────────────

Result<> WaterDepthSeedPass::create(Device& /*device*/, const PassResources& /*resources*/) {
    return {};
}

void WaterDepthSeedPass::destroy() {}

void WaterDepthSeedPass::post_create(GpuContext& /*gpu*/, PassRegistry& passes) {
    water_ = passes.try_get<WaterPass>("water");
}

PassSetup WaterDepthSeedPass::setup() const {
    return {
        .name = "water_depth_seed",
        .phase = RenderPhase::Color,
        .sub_order = 20,   // once the opaque scene and its overlays are drawn
    };
}

void WaterDepthSeedPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    if (!ctx.water_draws) { return; }
    builder.read(builder.import_named(ctx, "msaa_depth"), ResourceAccess::transfer_src);
    builder.write(builder.import_named(ctx, "msaa_surface_depth"), ResourceAccess::transfer_dst);
}

void WaterDepthSeedPass::record(const FrameContext& ctx) {
    if (water_ != nullptr) { water_->seed_water_depth(ctx); }
}

// ── WaterDepthPrepass ───────────────────────────────────────────────────────

Result<> WaterDepthPrepass::create(Device& /*device*/, const PassResources& /*resources*/) {
    return {};
}

void WaterDepthPrepass::destroy() {}

void WaterDepthPrepass::post_create(GpuContext& /*gpu*/, PassRegistry& passes) {
    water_ = passes.try_get<WaterPass>("water");
}

PassSetup WaterDepthPrepass::setup() const {
    return {
        .name = "water_depth_prepass",
        .phase = RenderPhase::Color,
        .sub_order = 21,   // right after the seed
    };
}

void WaterDepthPrepass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    if (!ctx.water_draws) { return; }
    // The seeded attachment, tested and written; under MSAA it resolves into
    // the surface depth as the rendering ends, and without MSAA the two are
    // one image.
    builder.write(builder.import_named(ctx, "msaa_surface_depth"),
                  ResourceAccess::depth_attachment);
    builder.write(builder.import_named(ctx, "surface_depth"),
                  ctx.msaa_samples != VK_SAMPLE_COUNT_1_BIT ? ResourceAccess::depth_resolve
                                                            : ResourceAccess::depth_attachment);
    // The simulated sea, which the mesh stage moves the surface by.
    WaterPass::declare_simulation_reads(builder, ctx, ResourceAccess::sampled_mesh);
}

void WaterDepthPrepass::record(const FrameContext& ctx) {
    if (water_ != nullptr) { water_->record_depth(ctx); }
}

} // namespace fjell
