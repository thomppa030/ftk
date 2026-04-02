#include "renderer/systems/particle_system.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/resources/shader_utils.hpp"
#include "renderer/systems/vfx_emitter.hpp"
#include "renderer/gpu/vk_check.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "scene/scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace fjell {

// Must match the GPU Particle struct in particle_common.glsl
constexpr VkDeviceSize PARTICLE_STRIDE = 80;

void ParticleSystem::create(Device& device, const PassResources& resources) {
    device_ = device.handle();
    physical_device_ = device.physical_device();
    global_set_layout_ = resources.global_set_layout;
    bindless_set_layout_ = resources.material_set_layout;
    color_format_ = resources.color_format;
    depth_format_ = resources.depth_format;
    sample_count_ = resources.sample_count;

    auto families = device.find_queue_families();
    VkCommandPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_ci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_ci.queueFamilyIndex = families.graphics.value();
    if (vkCreateCommandPool(device_, &pool_ci, nullptr, &command_pool_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create particle command pool");
    }
    queue_ = device.graphics_queue();

    // ── Compute descriptor set layout (6 bindings) ──
    // 0=particles, 1=counters, 2=dead_list, 3=emitter_data, 4=indirect, 5=modules

    VkDescriptorSetLayoutBinding compute_bindings[6]{};
    for (int i = 0; i < 6; ++i) {
        compute_bindings[i].binding = static_cast<uint32_t>(i);
        compute_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        compute_bindings[i].descriptorCount = 1;
        compute_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_ci.bindingCount = 6;
    layout_ci.pBindings = compute_bindings;
    if (vkCreateDescriptorSetLayout(device_, &layout_ci, nullptr, &compute_set_layout_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create particle compute set layout");
    }

    // ── Compute pipeline layout ──

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.size = sizeof(ParticleComputePush);

    VkPipelineLayoutCreateInfo pipe_layout_ci{};
    pipe_layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipe_layout_ci.setLayoutCount = 1;
    pipe_layout_ci.pSetLayouts = &compute_set_layout_;
    pipe_layout_ci.pushConstantRangeCount = 1;
    pipe_layout_ci.pPushConstantRanges = &push_range;
    if (vkCreatePipelineLayout(device_, &pipe_layout_ci, nullptr, &compute_layout_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create particle compute pipeline layout");
    }

    // ── Compute pipelines ──

    auto emit_mod = create_shader_module(device_, read_spirv(base_path::resolve("shaders/particle_emit.comp.spv")));
    auto sim_mod = create_shader_module(device_, read_spirv(base_path::resolve("shaders/particle_sim.comp.spv")));
    auto indirect_mod = create_shader_module(device_, read_spirv(base_path::resolve("shaders/particle_indirect.comp.spv")));

    auto make_compute = [&](VkShaderModule mod) {
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = mod;
        stage.pName = "main";

        VkComputePipelineCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ci.stage = stage;
        ci.layout = compute_layout_;

        VkPipeline pipeline;
        if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create particle compute pipeline");
        }
        return pipeline;
    };

    emit_pipeline_ = make_compute(emit_mod);
    sim_pipeline_ = make_compute(sim_mod);
    indirect_pipeline_ = make_compute(indirect_mod);

    vkDestroyShaderModule(device_, emit_mod, nullptr);
    vkDestroyShaderModule(device_, sim_mod, nullptr);
    vkDestroyShaderModule(device_, indirect_mod, nullptr);

    // ── Depth sampler for soft particles ──

    VkSamplerCreateInfo sampler_ci{};
    sampler_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_ci.magFilter = VK_FILTER_NEAREST;
    sampler_ci.minFilter = VK_FILTER_NEAREST;
    sampler_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device_, &sampler_ci, nullptr, &depth_sampler_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create particle depth sampler");
    }

    // ── Render descriptor set layout (particle SSBO + depth texture) ──

    VkDescriptorSetLayoutBinding render_bindings[2]{};
    render_bindings[0].binding = 0;
    render_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    render_bindings[0].descriptorCount = 1;
    render_bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    render_bindings[1].binding = 1;
    render_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    render_bindings[1].descriptorCount = 1;
    render_bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo render_layout_ci{};
    render_layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    render_layout_ci.bindingCount = 2;
    render_layout_ci.pBindings = render_bindings;
    if (vkCreateDescriptorSetLayout(device_, &render_layout_ci, nullptr, &render_set_layout_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create particle render set layout");
    }

    // ── Render pipeline layout (set 0 = global, set 1 = particle SSBO + depth, set 2 = bindless textures) ──

    VkDescriptorSetLayout render_sets[] = {global_set_layout_, render_set_layout_, bindless_set_layout_};
    VkPipelineLayoutCreateInfo render_pipe_ci{};
    render_pipe_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    render_pipe_ci.setLayoutCount = 3;
    render_pipe_ci.pSetLayouts = render_sets;
    if (vkCreatePipelineLayout(device_, &render_pipe_ci, nullptr, &render_layout_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create particle render pipeline layout");
    }

    // ── Render pipelines (additive + alpha blend) ──

    auto vert_mod = create_shader_module(device_, read_spirv(base_path::resolve("shaders/particle.vert.spv")));
    auto frag_mod = create_shader_module(device_, read_spirv(base_path::resolve("shaders/particle.frag.spv")));

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
    multisampling.rasterizationSamples = sample_count_;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_FALSE;
    depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkFormat color_formats[] = {color_format_, VK_FORMAT_R8G8_UNORM};

    VkPipelineRenderingCreateInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_info.colorAttachmentCount = 2;
    rendering_info.pColorAttachmentFormats = color_formats;
    rendering_info.depthAttachmentFormat = depth_format_;

    auto create_render_pipeline = [&](VkBlendFactor dst_color) {
        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend.dstColorBlendFactor = dst_color;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;

        VkPipelineColorBlendAttachmentState mr_blend{};
        mr_blend.colorWriteMask = 0; // Particles don't write MR
        mr_blend.blendEnable = VK_FALSE;

        VkPipelineColorBlendAttachmentState blend_attachments[] = {blend, mr_blend};

        VkPipelineColorBlendStateCreateInfo color_blending{};
        color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        color_blending.attachmentCount = 2;
        color_blending.pAttachments = blend_attachments;

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
        ci.layout = render_layout_;

        VkPipeline pipeline;
        if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create particle render pipeline");
        }
        return pipeline;
    };

    render_additive_ = create_render_pipeline(VK_BLEND_FACTOR_ONE);
    render_alpha_ = create_render_pipeline(VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);

    vkDestroyShaderModule(device_, vert_mod, nullptr);
    vkDestroyShaderModule(device_, frag_mod, nullptr);

    // ── Descriptor pool (2 sets: compute + render) ──

    constexpr uint32_t RENDER_SET_COUNT = MAX_FRAMES_IN_FLIGHT;
    VkDescriptorPoolSize pool_sizes[2]{};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_sizes[0].descriptorCount = 6 + RENDER_SET_COUNT; // 6 compute + 1 per render frame
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_sizes[1].descriptorCount = RENDER_SET_COUNT; // depth texture per frame

    VkDescriptorPoolCreateInfo desc_pool_ci{};
    desc_pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    desc_pool_ci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    desc_pool_ci.maxSets = 1 + RENDER_SET_COUNT; // 1 compute + N render
    desc_pool_ci.poolSizeCount = 2;
    desc_pool_ci.pPoolSizes = pool_sizes;
    if (vkCreateDescriptorPool(device_, &desc_pool_ci, nullptr, &pool_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create particle descriptor pool");
    }

    FJELL_GFX_DEBUG("Particle system created");
}

void ParticleSystem::recreate_pipelines() {
    // Destroy old pipelines (layouts stay the same)
    auto safe_destroy = [&](auto& handle, auto destroy_fn) {
        if (handle != VK_NULL_HANDLE) { destroy_fn(device_, handle, nullptr); handle = VK_NULL_HANDLE; }
    };
    safe_destroy(emit_pipeline_, vkDestroyPipeline);
    safe_destroy(sim_pipeline_, vkDestroyPipeline);
    safe_destroy(indirect_pipeline_, vkDestroyPipeline);
    safe_destroy(render_additive_, vkDestroyPipeline);
    safe_destroy(render_alpha_, vkDestroyPipeline);

    // Recreate compute pipelines from recompiled SPIR-V
    auto make_compute = [&](const std::string& path) {
        auto mod = create_shader_module(device_, read_spirv(base_path::resolve(path)));
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = mod;
        stage.pName = "main";

        VkComputePipelineCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ci.stage = stage;
        ci.layout = compute_layout_;

        VkPipeline pipeline;
        if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline) != VK_SUCCESS) {
            FJELL_GFX_ERROR("Failed to recreate particle compute pipeline: {}", path);
            vkDestroyShaderModule(device_, mod, nullptr);
            return VkPipeline{VK_NULL_HANDLE};
        }
        vkDestroyShaderModule(device_, mod, nullptr);
        return pipeline;
    };

    emit_pipeline_ = make_compute("shaders/particle_emit.comp.spv");
    sim_pipeline_ = make_compute("shaders/particle_sim.comp.spv");
    indirect_pipeline_ = make_compute("shaders/particle_indirect.comp.spv");

    // Recreate render pipelines
    auto vert_mod = create_shader_module(device_, read_spirv(base_path::resolve("shaders/particle.vert.spv")));
    auto frag_mod = create_shader_module(device_, read_spirv(base_path::resolve("shaders/particle.frag.spv")));

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
    multisampling.rasterizationSamples = sample_count_;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_FALSE;
    depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkFormat color_formats[] = {color_format_, VK_FORMAT_R8G8_UNORM};

    VkPipelineRenderingCreateInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_info.colorAttachmentCount = 2;
    rendering_info.pColorAttachmentFormats = color_formats;
    rendering_info.depthAttachmentFormat = depth_format_;

    auto create_render_pipeline = [&](VkBlendFactor dst_color) {
        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend.dstColorBlendFactor = dst_color;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;

        VkPipelineColorBlendAttachmentState mr_blend{};
        mr_blend.colorWriteMask = 0;
        mr_blend.blendEnable = VK_FALSE;

        VkPipelineColorBlendAttachmentState blend_attachments[] = {blend, mr_blend};

        VkPipelineColorBlendStateCreateInfo color_blending{};
        color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        color_blending.attachmentCount = 2;
        color_blending.pAttachments = blend_attachments;

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
        ci.layout = render_layout_;

        VkPipeline pipeline;
        if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline) != VK_SUCCESS) {
            FJELL_GFX_ERROR("Failed to recreate particle render pipeline");
            return VkPipeline{VK_NULL_HANDLE};
        }
        return pipeline;
    };

    render_additive_ = create_render_pipeline(VK_BLEND_FACTOR_ONE);
    render_alpha_ = create_render_pipeline(VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);

    vkDestroyShaderModule(device_, vert_mod, nullptr);
    vkDestroyShaderModule(device_, frag_mod, nullptr);

    FJELL_GFX_DEBUG("Particle pipelines reloaded");
}

void ParticleSystem::destroy() {
    mega_particle_buffer_.reset();
    mega_counter_buffer_.reset();
    mega_dead_list_buffer_.reset();
    mega_indirect_buffer_.reset();
    emitter_data_buffer_.reset();
    module_buffer_.reset();
    emitters_.clear();

    auto safe_destroy = [&](auto& handle, auto destroy_fn) {
        if (handle != VK_NULL_HANDLE) { destroy_fn(device_, handle, nullptr); handle = VK_NULL_HANDLE; }
    };

    safe_destroy(depth_sampler_, vkDestroySampler);
    safe_destroy(pool_, vkDestroyDescriptorPool);
    safe_destroy(render_alpha_, vkDestroyPipeline);
    safe_destroy(render_additive_, vkDestroyPipeline);
    safe_destroy(render_layout_, vkDestroyPipelineLayout);
    safe_destroy(render_set_layout_, vkDestroyDescriptorSetLayout);
    safe_destroy(indirect_pipeline_, vkDestroyPipeline);
    safe_destroy(sim_pipeline_, vkDestroyPipeline);
    safe_destroy(emit_pipeline_, vkDestroyPipeline);
    safe_destroy(compute_layout_, vkDestroyPipelineLayout);
    safe_destroy(compute_set_layout_, vkDestroyDescriptorSetLayout);
    safe_destroy(command_pool_, vkDestroyCommandPool);
}

void ParticleSystem::rebuild_mega_buffers(uint32_t total_particles, uint32_t emitter_count) {
    // Save old buffers for data copy
    auto old_particle = std::move(mega_particle_buffer_);
    auto old_counter = std::move(mega_counter_buffer_);
    auto old_dead_list = std::move(mega_dead_list_buffer_);
    auto old_indirect = std::move(mega_indirect_buffer_);
    uint32_t old_particle_cap = total_particle_capacity_;
    uint32_t old_emitter_cap = emitter_capacity_;

    // Free old descriptor sets if they exist
    if (compute_set_ != VK_NULL_HANDLE) {
        vkFreeDescriptorSets(device_, pool_, 1, &compute_set_);
        compute_set_ = VK_NULL_HANDLE;
    }
    for (auto& rs : render_sets_) {
        if (rs != VK_NULL_HANDLE) {
            vkFreeDescriptorSets(device_, pool_, 1, &rs);
            rs = VK_NULL_HANDLE;
        }
    }

    total_particle_capacity_ = total_particles;
    emitter_capacity_ = emitter_count;

    constexpr auto STORAGE_SRC_DST = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    VkDeviceSize particle_size = static_cast<VkDeviceSize>(total_particles) * PARTICLE_STRIDE;
    VkDeviceSize counter_size = static_cast<VkDeviceSize>(emitter_count) * 2 * sizeof(uint32_t);
    VkDeviceSize dead_list_size = static_cast<VkDeviceSize>(total_particles) * sizeof(uint32_t);
    VkDeviceSize indirect_size = static_cast<VkDeviceSize>(emitter_count) * 4 * sizeof(uint32_t);
    VkDeviceSize emitter_data_size = static_cast<VkDeviceSize>(emitter_count) * sizeof(GpuEmitterData);

    mega_particle_buffer_ = std::make_unique<Buffer>(
        device_, physical_device_, particle_size,
        STORAGE_SRC_DST, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    mega_counter_buffer_ = std::make_unique<Buffer>(
        device_, physical_device_, counter_size,
        STORAGE_SRC_DST, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    mega_dead_list_buffer_ = std::make_unique<Buffer>(
        device_, physical_device_, dead_list_size,
        STORAGE_SRC_DST, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    mega_indirect_buffer_ = std::make_unique<Buffer>(
        device_, physical_device_, indirect_size,
        STORAGE_SRC_DST | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    // Copy old data to new buffers if we had previous data
    if (old_particle && old_particle_cap > 0) {
        VkCommandBufferAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandPool = command_pool_;
        alloc_info.commandBufferCount = 1;

        VkCommandBuffer cmd;
        vk_check(vkAllocateCommandBuffers(device_, &alloc_info, &cmd), "alloc copy cmd");

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk_check(vkBeginCommandBuffer(cmd, &begin), "begin copy cmd");

        VkDeviceSize old_particle_bytes = static_cast<VkDeviceSize>(old_particle_cap) * PARTICLE_STRIDE;
        VkBufferCopy region{0, 0, old_particle_bytes};
        vkCmdCopyBuffer(cmd, old_particle->handle(), mega_particle_buffer_->handle(), 1, &region);

        VkDeviceSize old_counter_bytes = static_cast<VkDeviceSize>(old_emitter_cap) * 2 * sizeof(uint32_t);
        region = {0, 0, old_counter_bytes};
        vkCmdCopyBuffer(cmd, old_counter->handle(), mega_counter_buffer_->handle(), 1, &region);

        VkDeviceSize old_dead_bytes = static_cast<VkDeviceSize>(old_particle_cap) * sizeof(uint32_t);
        region = {0, 0, old_dead_bytes};
        vkCmdCopyBuffer(cmd, old_dead_list->handle(), mega_dead_list_buffer_->handle(), 1, &region);

        VkDeviceSize old_indirect_bytes = static_cast<VkDeviceSize>(old_emitter_cap) * 4 * sizeof(uint32_t);
        region = {0, 0, old_indirect_bytes};
        vkCmdCopyBuffer(cmd, old_indirect->handle(), mega_indirect_buffer_->handle(), 1, &region);

        vk_check(vkEndCommandBuffer(cmd), "end copy cmd");

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        vk_check(vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE), "submit copy cmd");
        vkQueueWaitIdle(queue_);

        vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
    }

    emitter_data_buffer_ = std::make_unique<Buffer>(
        device_, physical_device_, emitter_data_size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    emitter_data_buffer_->map();

    // Module buffer: 1024 bytes per emitter (largest module is 72 bytes,
    // emitters can have 10+ modules, so 512 is too small)
    VkDeviceSize module_size = static_cast<VkDeviceSize>(emitter_count) * 1024;
    module_buffer_ = std::make_unique<Buffer>(
        device_, physical_device_, module_size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    module_buffer_->map();

    // ── Allocate descriptor sets ──

    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = pool_;
    alloc.descriptorSetCount = 1;

    alloc.pSetLayouts = &compute_set_layout_;
    if (vkAllocateDescriptorSets(device_, &alloc, &compute_set_) != VK_SUCCESS) {
        FJELL_GFX_ERROR("Failed to allocate particle compute descriptor set");
        return;
    }

    // Allocate per-frame render descriptor sets
    VkDescriptorSetLayout render_layouts[MAX_FRAMES_IN_FLIGHT];
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        render_layouts[i] = render_set_layout_;
    }
    alloc.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    alloc.pSetLayouts = render_layouts;
    if (vkAllocateDescriptorSets(device_, &alloc, render_sets_.data()) != VK_SUCCESS) {
        FJELL_GFX_ERROR("Failed to allocate particle render descriptor sets");
        return;
    }

    // ── Write descriptors ──

    VkDescriptorBufferInfo particle_info{mega_particle_buffer_->handle(), 0, mega_particle_buffer_->size()};
    VkDescriptorBufferInfo counter_info{mega_counter_buffer_->handle(), 0, mega_counter_buffer_->size()};
    VkDescriptorBufferInfo dead_list_info{mega_dead_list_buffer_->handle(), 0, mega_dead_list_buffer_->size()};
    VkDescriptorBufferInfo emitter_info{emitter_data_buffer_->handle(), 0, emitter_data_buffer_->size()};
    VkDescriptorBufferInfo indirect_info{mega_indirect_buffer_->handle(), 0, mega_indirect_buffer_->size()};
    VkDescriptorBufferInfo module_info{module_buffer_->handle(), 0, module_buffer_->size()};

    // Compute set: 6 bindings
    VkWriteDescriptorSet writes[6]{};
    for (auto& w : writes) w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;

    writes[0].dstSet = compute_set_; writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].descriptorCount = 1; writes[0].pBufferInfo = &particle_info;

    writes[1].dstSet = compute_set_; writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].descriptorCount = 1; writes[1].pBufferInfo = &counter_info;

    writes[2].dstSet = compute_set_; writes[2].dstBinding = 2;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].descriptorCount = 1; writes[2].pBufferInfo = &dead_list_info;

    writes[3].dstSet = compute_set_; writes[3].dstBinding = 3;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[3].descriptorCount = 1; writes[3].pBufferInfo = &emitter_info;

    writes[4].dstSet = compute_set_; writes[4].dstBinding = 4;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[4].descriptorCount = 1; writes[4].pBufferInfo = &indirect_info;

    writes[5].dstSet = compute_set_; writes[5].dstBinding = 5;
    writes[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[5].descriptorCount = 1; writes[5].pBufferInfo = &module_info;

    vkUpdateDescriptorSets(device_, 6, writes, 0, nullptr);

    // Render sets: write particle SSBO (binding 0) to each frame's set
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        VkWriteDescriptorSet render_write{};
        render_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        render_write.dstSet = render_sets_[i];
        render_write.dstBinding = 0;
        render_write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        render_write.descriptorCount = 1;
        render_write.pBufferInfo = &particle_info;
        vkUpdateDescriptorSets(device_, 1, &render_write, 0, nullptr);
    }

    FJELL_GFX_DEBUG("Particle mega-buffers rebuilt: {} particles, {} emitters",
                    total_particles, emitter_count);
}

void ParticleSystem::init_emitter_ranges() {
    init_emitter_range(0, static_cast<uint32_t>(emitters_.size()));
}

void ParticleSystem::init_emitter_range(uint32_t from, uint32_t to) {
    if (from >= to) return;

    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandPool = command_pool_;
    alloc_info.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vk_check(vkAllocateCommandBuffers(device_, &alloc_info, &cmd),
             "Failed to allocate particle init command buffer");

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(cmd, &begin_info),
             "Failed to begin particle init command buffer");

    // Zero particle buffer and indirect draw commands for the target range
    for (uint32_t i = from; i < to; ++i) {
        const auto& em = emitters_[i];
        VkDeviceSize particle_offset = static_cast<VkDeviceSize>(em.base_offset) * PARTICLE_STRIDE;
        VkDeviceSize particle_size = static_cast<VkDeviceSize>(em.max_particles) * PARTICLE_STRIDE;
        vkCmdFillBuffer(cmd, mega_particle_buffer_->handle(), particle_offset, particle_size, 0);

        VkDeviceSize indirect_offset = static_cast<VkDeviceSize>(i) * 4 * sizeof(uint32_t);
        vkCmdFillBuffer(cmd, mega_indirect_buffer_->handle(), indirect_offset, 4 * sizeof(uint32_t), 0);
    }

    // Initialize dead lists per emitter
    constexpr VkDeviceSize max_update = 65536;
    for (uint32_t i = from; i < to; ++i) {
        const auto& em = emitters_[i];
        std::vector<uint32_t> indices(em.max_particles);
        for (uint32_t j = 0; j < em.max_particles; ++j) indices[j] = j;

        VkDeviceSize offset = static_cast<VkDeviceSize>(em.base_offset) * sizeof(uint32_t);
        VkDeviceSize remaining = static_cast<VkDeviceSize>(em.max_particles) * sizeof(uint32_t);
        const auto* src = reinterpret_cast<const char*>(indices.data());

        while (remaining > 0) {
            VkDeviceSize chunk = std::min(remaining, max_update);
            vkCmdUpdateBuffer(cmd, mega_dead_list_buffer_->handle(), offset, chunk, src);
            offset += chunk;
            src += chunk;
            remaining -= chunk;
        }
    }

    // Initialize counters for the target range only
    for (uint32_t i = from; i < to; ++i) {
        uint32_t counters[2] = {0, emitters_[i].max_particles};
        VkDeviceSize counter_offset = static_cast<VkDeviceSize>(i) * 2 * sizeof(uint32_t);
        vkCmdUpdateBuffer(cmd, mega_counter_buffer_->handle(), counter_offset,
                          sizeof(counters), counters);
    }

    vk_check(vkEndCommandBuffer(cmd), "Failed to end particle init command buffer");

    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd;

    vk_check(vkQueueSubmit(queue_, 1, &submit_info, VK_NULL_HANDLE),
             "Failed to submit particle init commands");
    vkQueueWaitIdle(queue_);

    vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
}

void ParticleSystem::sync_emitters(Scene& scene) {
    FJELL_PROFILE_SCOPE_N("particle_sync_emitters");

    // Collect current scene emitters
    struct EmitterKey {
        uint32_t object_id;
        uint32_t emitter_index;
        uint32_t max_particles;
        ParticleBlendMode blend_mode;
    };
    std::vector<EmitterKey> scene_emitters;
    for (const auto& obj : scene.objects()) {
        auto* ec = obj->components.get<EmitterComponent>();
        if (!ec) continue;
        if (!ec->effect) continue;
        for (uint32_t i = 0; i < static_cast<uint32_t>(ec->effect->emitters.size()); ++i) {
            auto& sub = ec->effect->emitters[i];
            scene_emitters.push_back({obj->id, i, sub.max_particles, sub.blend_mode});
        }
    }

    auto key_match = [](const EmitterGpu& gpu, const EmitterKey& key) {
        return gpu.object_id == key.object_id && gpu.emitter_index == key.emitter_index;
    };

    // Remove dead emitters
    for (auto it = emitters_.begin(); it != emitters_.end();) {
        bool alive = false;
        for (const auto& se : scene_emitters) {
            if (key_match(*it, se)) { alive = true; break; }
        }
        if (!alive) {
            it = emitters_.erase(it);
        } else {
            ++it;
        }
    }

    // Find new emitters not in the current list
    std::vector<EmitterKey> new_emitters;
    for (const auto& se : scene_emitters) {
        bool found = false;
        for (const auto& existing : emitters_) {
            if (key_match(existing, se)) { found = true; break; }
        }
        if (!found) new_emitters.push_back(se);
    }

    if (new_emitters.empty()) return;

    FJELL_GFX_DEBUG("Particle sync: {} existing, {} new",
                    emitters_.size(), new_emitters.size());

    // Compute next free offset (end of all existing allocations)
    uint32_t next_offset = 0;
    for (const auto& em : emitters_) {
        uint32_t end = em.base_offset + em.max_particles;
        if (end > next_offset) next_offset = end;
    }

    // Append new emitters (order must be stable — particles store emitter_index
    // at emit time and use it to look up EmitterData in the sim shader)
    uint32_t first_new = static_cast<uint32_t>(emitters_.size());
    for (const auto& ne : new_emitters) {
        EmitterGpu gpu{};
        gpu.object_id = ne.object_id;
        gpu.emitter_index = ne.emitter_index;
        gpu.max_particles = ne.max_particles;
        gpu.base_offset = next_offset;
        gpu.blend_mode = ne.blend_mode;
        emitters_.push_back(gpu);
        next_offset += ne.max_particles;
    }

    uint32_t total = next_offset;
    uint32_t count = static_cast<uint32_t>(emitters_.size());

    bool needs_realloc = total > total_particle_capacity_ || count > emitter_capacity_;

    if (needs_realloc) {
        vkDeviceWaitIdle(device_);
        uint32_t new_total = std::max(total, total_particle_capacity_ * 2);
        uint32_t new_count = std::max(count, emitter_capacity_ * 2);
        if (new_total == 0) new_total = total;
        if (new_count == 0) new_count = count;
        rebuild_mega_buffers(new_total, new_count);
        init_emitter_range(first_new, count);
    } else {
        vkQueueWaitIdle(queue_);
        init_emitter_range(first_new, count);
    }
}

void ParticleSystem::dispatch_compute(VkCommandBuffer cmd, float dt, Scene& scene) {
    FJELL_PROFILE_SCOPE_N("particle_dispatch_compute");
    if (emitters_.empty()) return;
    ++frame_seed_;

    uint32_t emitter_count = static_cast<uint32_t>(emitters_.size());
    uint32_t total_max = 0;
    for (const auto& em : emitters_) total_max += em.max_particles;

    // Build emitter metadata
    emitter_data_cache_.clear();
    emitter_data_cache_.resize(emitter_count);
    uint32_t max_emit_count = 0;

    auto* module_ptr = static_cast<uint8_t*>(module_buffer_->mapped());
    uint32_t module_word_offset = 0;

    for (size_t i = 0; i < emitters_.size(); ++i) {
        auto& gpu = emitters_[i];
        auto* obj = scene.find(gpu.object_id);
        auto& data = emitter_data_cache_[i];
        std::memset(&data, 0, sizeof(data));

        auto* ec = obj ? obj->components.get<EmitterComponent>() : nullptr;
        if (!ec || !ec->effect || !obj->as_spatial()
            || gpu.emitter_index >= ec->effect->emitters.size()) {
            // Emitter not valid — assign a terminator-only module block
            // so the shader exits the module walk immediately.
            data.module_offset = module_word_offset;
            auto* term = reinterpret_cast<uint32_t*>(module_ptr + module_word_offset * 4);
            term[0] = 0xFFFFFFFF; // MOD_TERMINATOR
            term[1] = 0;
            module_word_offset += 2;
            continue;
        }
        auto* spatial = obj->as_spatial();

        auto& sub = ec->effect->emitters[gpu.emitter_index];

        // CPU-side emission accumulation (per sub-emitter)
        float eff_dt = sub.paused ? 0.0f : dt;
        uint32_t emit_count = 0;
        if (!sub.paused) {
            uint32_t burst = sub.burst_count();
            if (burst > 0 && !sub.burst_fired) {
                emit_count = burst;
                sub.burst_fired = true;
            }
            sub.emit_accumulator += sub.emission_rate() * dt;
            emit_count += static_cast<uint32_t>(sub.emit_accumulator);
            sub.emit_accumulator -= std::floor(sub.emit_accumulator);
        }
        if (emit_count > max_emit_count) max_emit_count = emit_count;

        data.emitter_position = glm::vec3(spatial->world_matrix_[3]);
        data.dt = eff_dt;
        data.gravity = {}; // read from modules now
        data.max_particles = gpu.max_particles;
        data.color_start = {}; // read from modules
        data.color_end = {};
        data.size_start = 0.0f;
        data.size_end = 0.0f;
        data.lifetime_min = sub.lifetime_min();
        data.lifetime_max = sub.lifetime_max();
        data.velocity_min = {};
        data.emit_count = emit_count;
        data.velocity_max = {};
        data.random_seed = frame_seed_ ^ ((gpu.object_id * 31 + gpu.emitter_index) * 2654435761u);
        data.base_offset = gpu.base_offset;
        data.counter_offset = static_cast<uint32_t>(i) * 2;
        data.dead_list_offset = gpu.base_offset;

        // Write module data
        data.module_offset = module_word_offset;
        uint32_t module_buf_capacity = static_cast<uint32_t>(module_buffer_->size());
        uint32_t byte_offset = module_word_offset * 4;
        if (byte_offset + 1024 > module_buf_capacity) {
            FJELL_GFX_ERROR("Module buffer overflow: offset {} + 1024 > capacity {}",
                            byte_offset, module_buf_capacity);
            continue;
        }
        uint32_t bytes = sub.write_gpu_modules(module_ptr + byte_offset);
        module_word_offset += bytes / 4;
    }

    // Upload emitter data
    std::memcpy(emitter_data_buffer_->mapped(), emitter_data_cache_.data(),
                emitter_count * sizeof(GpuEmitterData));

    ParticleComputePush push{};
    push.dt = dt;
    push.random_seed = frame_seed_;
    push.emitter_count = emitter_count;
    push.total_max_particles = total_max;

    auto compute_barrier = [&]() {
        VkMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;

        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);
    };

    // ── Phase 1: Emit (single dispatch) ──
    if (max_emit_count > 0) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, emit_pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compute_layout_,
                                0, 1, &compute_set_, 0, nullptr);
        vkCmdPushConstants(cmd, compute_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(push), &push);
        vkCmdDispatch(cmd, (max_emit_count + 255) / 256, emitter_count, 1);
        compute_barrier();
    }

    // ── Phase 2: Simulate (single dispatch) ──
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sim_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compute_layout_,
                            0, 1, &compute_set_, 0, nullptr);
    vkCmdPushConstants(cmd, compute_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);
    vkCmdDispatch(cmd, (total_max + 255) / 256, 1, 1);
    compute_barrier();

    // ── Phase 3: Indirect prep (single dispatch) ──
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, indirect_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compute_layout_,
                            0, 1, &compute_set_, 0, nullptr);
    vkCmdPushConstants(cmd, compute_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);
    vkCmdDispatch(cmd, (emitter_count + 255) / 256, 1, 1);
}

void ParticleSystem::insert_barriers(VkCommandBuffer cmd) {
    if (emitters_.empty()) return;

    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;

    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void ParticleSystem::record_render(const FrameContext& ctx) {
    FJELL_PROFILE_SCOPE_N("particle_record_render");
    if (emitters_.empty()) return;

    // Update this frame's depth texture descriptor (safe: fence waited before recording)
    VkDescriptorSet frame_set = render_sets_[ctx.frame_index];

    VkDescriptorImageInfo depth_info{};
    depth_info.sampler = depth_sampler_;
    depth_info.imageView = ctx.target->depth_view();
    depth_info.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet depth_write{};
    depth_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    depth_write.dstSet = frame_set;
    depth_write.dstBinding = 1;
    depth_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    depth_write.descriptorCount = 1;
    depth_write.pImageInfo = &depth_info;
    vkUpdateDescriptorSets(device_, 1, &depth_write, 0, nullptr);

    // Bind global + particle set (SSBO + depth) + bindless textures
    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, render_layout_,
                            0, 1, &ctx.global_descriptor_set, 0, nullptr);
    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, render_layout_,
                            1, 1, &frame_set, 0, nullptr);
    if (ctx.bindless_set != VK_NULL_HANDLE) {
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, render_layout_,
                                2, 1, &ctx.bindless_set, 0, nullptr);
    }

    VkViewport vp{};
    vp.width = static_cast<float>(ctx.target->extent().width);
    vp.height = static_cast<float>(ctx.target->extent().height);
    vp.maxDepth = 1.0f;

    VkRect2D sc{};
    sc.extent = ctx.target->extent();

    constexpr uint32_t stride = sizeof(VkDrawIndirectCommand);

    // Draw each emitter individually with its blend mode pipeline.
    // Emitters can be in any order — particles store emitter_index at emit time,
    // so we can't reorder the emitters_ vector. Instead we batch consecutive
    // runs of the same blend mode into single multi-draw indirect calls.
    VkPipeline current_pipeline = VK_NULL_HANDLE;
    uint32_t batch_start = 0;
    uint32_t batch_count = 0;

    auto flush_batch = [&]() {
        if (batch_count == 0) return;
        vkCmdDrawIndirect(ctx.cmd, mega_indirect_buffer_->handle(),
                          batch_start * stride, batch_count, stride);
        batch_count = 0;
    };

    for (uint32_t i = 0; i < static_cast<uint32_t>(emitters_.size()); ++i) {
        VkPipeline needed = (emitters_[i].blend_mode == ParticleBlendMode::additive)
            ? render_additive_ : render_alpha_;

        if (needed != current_pipeline) {
            flush_batch();
            current_pipeline = needed;
            vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, current_pipeline);
            vkCmdSetViewport(ctx.cmd, 0, 1, &vp);
            vkCmdSetScissor(ctx.cmd, 0, 1, &sc);
            batch_start = i;
        }

        if (batch_count == 0) batch_start = i;
        ++batch_count;
    }
    flush_batch();
}

} // namespace fjell
