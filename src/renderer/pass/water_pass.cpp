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
#include "renderer/render_stats.hpp"
#include "renderer/pass/pass_gpu_zone.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/resources/material.hpp"
#include "renderer/resources/shader_utils.hpp"
#include "renderer/resources/water_gpu.hpp"
#include "renderer/resources/water_surface.hpp"
#include "renderer/uniform_types.hpp"
#include "scene/scene.hpp"

#include <array>
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
    std::array<VkDescriptorSetLayoutBinding, 5> set_bindings{};
    set_bindings[0].binding = 0;
    set_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    set_bindings[0].descriptorCount = 1;
    set_bindings[0].stageFlags = VK_SHADER_STAGE_MESH_BIT_EXT;
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
        set_bindings[binding].stageFlags = VK_SHADER_STAGE_MESH_BIT_EXT;
    }

    VkDescriptorSetLayoutCreateInfo set_layout_info{};
    set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout_info.bindingCount = static_cast<uint32_t>(set_bindings.size());
    set_layout_info.pBindings = set_bindings.data();
    if (vkCreateDescriptorSetLayout(device_, &set_layout_info, nullptr,
                                    &water_set_layout_) != VK_SUCCESS) {
        return make_error("Failed to create water set layout");
    }

    field_buffer_ = std::make_unique<Buffer>(
        allocator_, sizeof(GpuWaterField), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT);
    clipmap_buffer_ = std::make_unique<Buffer>(
        allocator_, sizeof(GpuWaterClipmap), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT);

    // A field of flat water until a node supplies one, so the buffer is never
    // read as uninitialised memory. write() maps on first use and keeps the
    // mapping; the buffer is host-visible and rewritten whenever a sea is
    // edited, so it stays mapped for the pass's life.
    GpuWaterField empty{};
    field_buffer_->write(&empty, sizeof(empty));
    bound_field_ = empty;

    bound_clipmap_ = clipmap_.gpu();
    clipmap_buffer_->write(&bound_clipmap_, sizeof(bound_clipmap_));

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

    return {};
}

void WaterPass::destroy() {
    destroy_material_pipelines();
    field_buffer_.reset();
    clipmap_buffer_.reset();
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
    bound_water_ = nullptr;
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
    if (simulation_ == nullptr) {
        FJELL_CORE_ERROR("WaterPass: no water_fft pass in the render graph. Water samples the "
                         "simulated sea's textures whether or not a field has one, so without "
                         "that pass it cannot draw.");
    }
}

void WaterPass::prepare_frame(const FrameContext& ctx, GpuContext& /*gpu*/) {
    if (ctx.scene == nullptr || pipeline_cache_ == nullptr) return;

    // The levels follow the camera, not any particular body of water, so
    // they are recentered once for the frame regardless of how many infinite
    // seas the scene holds.
    clipmap_.recenter(ctx.camera_position);

    // The world runs one simulated sea: the first a water node asks for.
    if (simulation_ != nullptr) {
        std::optional<FftContributor> sea;
        for (const auto& node : ctx.scene->objects()) {
            const auto* wc = node->components.get<WaterComponent>();
            if (wc == nullptr) continue;
            if (const auto* fft = water::simulated_sea(wc->effective_water)) {
                sea = *fft;
                break;
            }
        }
        simulation_->set_sea(sea, ctx.thread_pool);
    }

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

    // Tests depth so the sea is hidden by what stands in front of it, and
    // writes it so the sea can hide itself.
    //
    // A Gerstner surface displaces horizontally, and where the trains pinch
    // past their limit it folds through itself: a crest then genuinely stands
    // in front of the water behind it. Without a depth write nothing ordered
    // the two and the fragments composited in submission order, which reads as
    // geometry going missing at the crests.
    //
    // The attachment this writes is the water's own — `water_depth`, seeded
    // from the scene depth before the scope opens — and not the depth the rest
    // of the frame samples. That separation is the whole design: a dozen
    // shaders read `depth_texture` meaning "the opaque scene", and a
    // transparent surface does not belong in it.
    //
    // LESS rather than LESS_OR_EQUAL: with writes on, LESS gives a
    // deterministic winner where two folded sheets land on the same depth,
    // while LESS_OR_EQUAL would hand the decision back to submission order —
    // which is the bug.
    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_TRUE;
    depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS;

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

void WaterPass::bind_water_field(const WaterComponent& comp) {
    // The editor rewrites a field as it is dragged, so comparing the pointer
    // is not enough to know nothing changed — compare what the shader will
    // actually read. Kept as a copy rather than read back out of the mapped
    // buffer: that memory is write-combined, and reading it is slow.
    GpuWaterField field = to_gpu_field(comp.effective_water);
    if (bound_water_ == &comp &&
        std::memcmp(&bound_field_, &field, sizeof(field)) == 0) {
        return;
    }
    field_buffer_->write(&field, sizeof(field));
    bound_field_ = field;
    bound_water_ = &comp;
}

VkDescriptorSet WaterPass::acquire_water_set(const FrameContext& ctx) const {
    auto sampled = [this](std::uint32_t binding, VkImageView view) {
        FrameCacheBinding b{};
        b.binding = binding;
        b.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.image.sampler = simulation_->sampler();
        b.image.imageView = view;
        b.image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        return b;
    };
    auto uniform = [](std::uint32_t binding, VkBuffer buffer, VkDeviceSize range) {
        FrameCacheBinding b{};
        b.binding = binding;
        b.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b.buffer.buffer = buffer;
        b.buffer.range = range;
        return b;
    };
    const FrameCacheBinding bindings[5]{
        uniform(0, field_buffer_->handle(), sizeof(GpuWaterField)),
        uniform(1, clipmap_buffer_->handle(), sizeof(GpuWaterClipmap)),
        sampled(2, simulation_->displacement_view()),
        sampled(3, simulation_->previous_displacement_view()),
        sampled(4, simulation_->derivatives_view()),
    };
    return ctx.descriptor_cache->acquire(water_set_layout_, {bindings, 5});
}

void WaterPass::bind_clipmap() {
    // Most frames move the inner levels and leave the outer ones alone, and
    // a camera standing still moves none of them. Compared against a copy
    // rather than read back from the buffer: that memory is write-combined,
    // and reading it is slow.
    const GpuWaterClipmap& table = clipmap_.gpu();
    if (std::memcmp(&bound_clipmap_, &table, sizeof(table)) == 0) {
        return;
    }
    clipmap_buffer_->write(&table, sizeof(table));
    bound_clipmap_ = table;
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

    FJELL_GPU_ZONE(ctx, "Water Depth Seed");

    const VkImageSubresourceRange depth_range{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};

    // The scene depth arrives read-only — the transparent pass declares it as
    // something it samples — and has to leave in the same layout, because the
    // scope that follows binds it that way and the framegraph believes it is
    // still there.
    std::array<VkImageMemoryBarrier2, 2> to_transfer{};
    to_transfer[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    to_transfer[0].srcStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    to_transfer[0].srcAccessMask = 0;
    to_transfer[0].dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    to_transfer[0].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    to_transfer[0].oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    to_transfer[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_transfer[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer[0].image = scene_depth;
    to_transfer[0].subresourceRange = depth_range;

    // Last frame's water depth is of no use to this one, so it is discarded
    // rather than transitioned: UNDEFINED tells the driver not to preserve it.
    to_transfer[1] = to_transfer[0];
    to_transfer[1].srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    to_transfer[1].srcAccessMask = 0;
    to_transfer[1].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    to_transfer[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_transfer[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_transfer[1].image = water_depth;

    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = static_cast<uint32_t>(to_transfer.size());
    dep.pImageMemoryBarriers = to_transfer.data();
    vkCmdPipelineBarrier2(ctx.cmd, &dep);

    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    region.dstSubresource = region.srcSubresource;
    region.extent = {ctx.target->extent().width, ctx.target->extent().height, 1};
    vkCmdCopyImage(ctx.cmd, scene_depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   water_depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    std::array<VkImageMemoryBarrier2, 2> to_use{};
    to_use[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    to_use[0].srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    to_use[0].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    to_use[0].dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    to_use[0].dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                              VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    to_use[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_use[0].newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    to_use[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_use[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_use[0].image = water_depth;
    to_use[0].subresourceRange = depth_range;

    // And the scene depth back where it was found.
    to_use[1] = to_use[0];
    to_use[1].srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    to_use[1].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                             VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
    to_use[1].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                              VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    to_use[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_use[1].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    to_use[1].image = scene_depth;

    dep.pImageMemoryBarriers = to_use.data();
    vkCmdPipelineBarrier2(ctx.cmd, &dep);
}

void WaterPass::record(const FrameContext& ctx) {
    if (!draws(ctx)) return;

    FJELL_PROFILE_SCOPE_N("record_water");
    FJELL_GPU_ZONE(ctx, "Water");

    auto* target = ctx.target;
    const bool msaa = target->sample_count() != VK_SAMPLE_COUNT_1_BIT;

    // The sea draws in a scope of its own, because it needs a depth attachment
    // it may write and the transparent pass binds the scene depth read-only —
    // a dozen shaders sample that image meaning "the opaque scene", which the
    // water is not part of. Same colour targets, loaded and stored, so the
    // transparent scope that follows carries on over the top of the water.
    //
    // No resolve here: the transparent scope resolves the same attachments at
    // its end, and that resolve sees the water's contribution because it
    // resolves the whole image. Resolving twice would be correct and wasted.
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

    // The water's own depth, holding the scene depth the seed copied into it:
    // loaded so the sea is still hidden by what stands in front of it.
    //
    // What is left in it at the end is the surface depth — the opaque scene
    // with the sea laid over it — for whatever asks how far away a pixel is
    // once the water is drawn. Under MSAA that leaves by a resolve and the
    // multisampled image itself is not kept; at one sample the attachment is
    // the surface depth and has to be stored.
    VkRenderingAttachmentInfo depth_attachment{};
    depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth_attachment.imageView = target->water_depth_view();
    depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    if (msaa) {
        depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth_attachment.resolveImageView = target->resolve_water_depth_view();
        depth_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth_attachment.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
    } else {
        depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth_attachment.resolveMode = VK_RESOLVE_MODE_NONE;
    }

    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.offset = {0, 0};
    ri.renderArea.extent = target->extent();
    ri.layerCount = 1;
    ri.colorAttachmentCount = static_cast<uint32_t>(color_attachments.size());
    ri.pColorAttachments = color_attachments.data();
    ri.pDepthAttachment = &depth_attachment;
    vkCmdBeginRendering(ctx.cmd, &ri);

    // Its own scope means its own viewport and scissor; it can no longer rely
    // on the transparent pass having set them.
    VkViewport vp{};
    vp.width = static_cast<float>(target->extent().width);
    vp.height = static_cast<float>(target->extent().height);
    vp.maxDepth = 1.0F;
    vkCmdSetViewport(ctx.cmd, 0, 1, &vp);

    VkRect2D sc{};
    sc.extent = target->extent();
    vkCmdSetScissor(ctx.cmd, 0, 1, &sc);

    bool bound_sets = false;
    VkPipeline current = VK_NULL_HANDLE;

    for (const auto& node : ctx.scene->objects()) {
        const auto* wc = node->components.get<WaterComponent>();
        if (wc == nullptr) continue;

        VkPipeline pipeline = VK_NULL_HANDLE;
        uint32_t material_id = 0;
        resolve_node_material(*wc, pipeline, material_id);
        if (pipeline == VK_NULL_HANDLE) continue;

        bind_water_field(*wc);
        bind_clipmap();

        if (!bound_sets) {
            std::array<VkDescriptorSet, 3> sets = {ctx.global_descriptor_set,
                                                  ctx.bindless_set, acquire_water_set(ctx)};
            vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0,
                                    static_cast<uint32_t>(sets.size()), sets.data(), 0,
                                    nullptr);
            bound_sets = true;
        }
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
        push.sdf_volume_min = sdf_volume_min_;
        push.sdf_volume_max = sdf_volume_max_;

        vkCmdPushConstants(ctx.cmd, layout_, WATER_PUSH_STAGES, 0, sizeof(push), &push);
        draw_mesh_tasks_(ctx.cmd, infinite ? WaterClipmap::patch_count() : patches * patches,
                         1, 1);

        if (ctx.render_stats != nullptr) {
            ctx.render_stats->draw_calls++;
        }
    }

    vkCmdEndRendering(ctx.cmd);
}

} // namespace fjell
