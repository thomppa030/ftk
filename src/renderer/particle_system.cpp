#include "renderer/particle_system.hpp"
#include "renderer/device.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/shader_utils.hpp"
#include "renderer/vk_check.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "scene/scene.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace fjell {

void ParticleSystem::create(Device& device, const PassResources& resources) {
    device_ = device.handle();
    physical_device_ = device.physical_device();
    global_set_layout_ = resources.global_set_layout;
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

    // ── Compute descriptor set layout (5 bindings) ──

    VkDescriptorSetLayoutBinding compute_bindings[5]{};
    for (int i = 0; i < 5; ++i) {
        compute_bindings[i].binding = static_cast<uint32_t>(i);
        compute_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        compute_bindings[i].descriptorCount = 1;
        compute_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_ci.bindingCount = 5;
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

    // ── Render descriptor set layout (particle SSBO) ──

    VkDescriptorSetLayoutBinding render_binding{};
    render_binding.binding = 0;
    render_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    render_binding.descriptorCount = 1;
    render_binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo render_layout_ci{};
    render_layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    render_layout_ci.bindingCount = 1;
    render_layout_ci.pBindings = &render_binding;
    if (vkCreateDescriptorSetLayout(device_, &render_layout_ci, nullptr, &render_set_layout_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create particle render set layout");
    }

    // ── Render pipeline layout (set 0 = global, set 1 = particle SSBO) ──

    VkDescriptorSetLayout render_sets[] = {global_set_layout_, render_set_layout_};
    VkPipelineLayoutCreateInfo render_pipe_ci{};
    render_pipe_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    render_pipe_ci.setLayoutCount = 2;
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

    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = 6; // 5 compute + 1 render

    VkDescriptorPoolCreateInfo desc_pool_ci{};
    desc_pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    desc_pool_ci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    desc_pool_ci.maxSets = 2;
    desc_pool_ci.poolSizeCount = 1;
    desc_pool_ci.pPoolSizes = &pool_size;
    if (vkCreateDescriptorPool(device_, &desc_pool_ci, nullptr, &pool_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create particle descriptor pool");
    }

    FJELL_GFX_DEBUG("Particle system created");
}

void ParticleSystem::destroy() {
    mega_particle_buffer_.reset();
    mega_counter_buffer_.reset();
    mega_dead_list_buffer_.reset();
    mega_indirect_buffer_.reset();
    emitter_data_buffer_.reset();
    emitters_.clear();

    auto safe_destroy = [&](auto& handle, auto destroy_fn) {
        if (handle != VK_NULL_HANDLE) { destroy_fn(device_, handle, nullptr); handle = VK_NULL_HANDLE; }
    };

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
    // Free old descriptor sets if they exist
    if (compute_set_ != VK_NULL_HANDLE) {
        vkFreeDescriptorSets(device_, pool_, 1, &compute_set_);
        compute_set_ = VK_NULL_HANDLE;
    }
    if (render_set_ != VK_NULL_HANDLE) {
        vkFreeDescriptorSets(device_, pool_, 1, &render_set_);
        render_set_ = VK_NULL_HANDLE;
    }

    total_particle_capacity_ = total_particles;
    emitter_capacity_ = emitter_count;

    VkDeviceSize particle_size = static_cast<VkDeviceSize>(total_particles) * 64;
    VkDeviceSize counter_size = static_cast<VkDeviceSize>(emitter_count) * 2 * sizeof(uint32_t);
    VkDeviceSize dead_list_size = static_cast<VkDeviceSize>(total_particles) * sizeof(uint32_t);
    VkDeviceSize indirect_size = static_cast<VkDeviceSize>(emitter_count) * 4 * sizeof(uint32_t);
    VkDeviceSize emitter_data_size = static_cast<VkDeviceSize>(emitter_count) * sizeof(GpuEmitterData);

    mega_particle_buffer_ = std::make_unique<Buffer>(
        device_, physical_device_, particle_size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    mega_counter_buffer_ = std::make_unique<Buffer>(
        device_, physical_device_, counter_size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    mega_dead_list_buffer_ = std::make_unique<Buffer>(
        device_, physical_device_, dead_list_size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    mega_indirect_buffer_ = std::make_unique<Buffer>(
        device_, physical_device_, indirect_size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    emitter_data_buffer_ = std::make_unique<Buffer>(
        device_, physical_device_, emitter_data_size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    emitter_data_buffer_->map();

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

    alloc.pSetLayouts = &render_set_layout_;
    if (vkAllocateDescriptorSets(device_, &alloc, &render_set_) != VK_SUCCESS) {
        FJELL_GFX_ERROR("Failed to allocate particle render descriptor set");
        return;
    }

    // ── Write descriptors ──

    VkDescriptorBufferInfo particle_info{mega_particle_buffer_->handle(), 0, mega_particle_buffer_->size()};
    VkDescriptorBufferInfo counter_info{mega_counter_buffer_->handle(), 0, mega_counter_buffer_->size()};
    VkDescriptorBufferInfo dead_list_info{mega_dead_list_buffer_->handle(), 0, mega_dead_list_buffer_->size()};
    VkDescriptorBufferInfo emitter_info{emitter_data_buffer_->handle(), 0, emitter_data_buffer_->size()};
    VkDescriptorBufferInfo indirect_info{mega_indirect_buffer_->handle(), 0, mega_indirect_buffer_->size()};

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

    writes[5].dstSet = render_set_; writes[5].dstBinding = 0;
    writes[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[5].descriptorCount = 1; writes[5].pBufferInfo = &particle_info;

    vkUpdateDescriptorSets(device_, 6, writes, 0, nullptr);

    FJELL_GFX_DEBUG("Particle mega-buffers rebuilt: {} particles, {} emitters",
                    total_particles, emitter_count);
}

void ParticleSystem::init_emitter_ranges() {
    // Initialize all emitter ranges in a single command buffer submission.
    // Uses vkCmdFillBuffer (zero particles/indirect) + vkCmdUpdateBuffer
    // (dead lists + counters) to avoid staging buffers and multiple queue waits.

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

    // Zero particle buffer and indirect buffer in bulk
    uint32_t total_particles = 0;
    for (const auto& em : emitters_) total_particles += em.max_particles;

    if (total_particles > 0) {
        vkCmdFillBuffer(cmd, mega_particle_buffer_->handle(), 0,
                        static_cast<VkDeviceSize>(total_particles) * 64, 0);
    }
    uint32_t emitter_count = static_cast<uint32_t>(emitters_.size());
    if (emitter_count > 0) {
        vkCmdFillBuffer(cmd, mega_indirect_buffer_->handle(), 0,
                        static_cast<VkDeviceSize>(emitter_count) * 4 * sizeof(uint32_t), 0);
    }

    // Initialize dead lists per emitter using vkCmdUpdateBuffer (max 65536 bytes per call)
    constexpr VkDeviceSize max_update = 65536;
    for (const auto& em : emitters_) {
        // Build local index sequence [0, 1, 2, ... max_particles-1]
        std::vector<uint32_t> indices(em.max_particles);
        for (uint32_t i = 0; i < em.max_particles; ++i) indices[i] = i;

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

    // Initialize counters: alive=0, dead=max_particles per emitter
    if (emitter_count > 0) {
        std::vector<uint32_t> counters(emitter_count * 2);
        for (size_t i = 0; i < emitters_.size(); ++i) {
            counters[i * 2 + 0] = 0;
            counters[i * 2 + 1] = emitters_[i].max_particles;
        }
        VkDeviceSize size = static_cast<VkDeviceSize>(emitter_count) * 2 * sizeof(uint32_t);
        vkCmdUpdateBuffer(cmd, mega_counter_buffer_->handle(), 0, size, counters.data());
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

void ParticleSystem::sync_emitters(const Scene& scene) {
    FJELL_PROFILE_SCOPE_N("particle_sync_emitters");

    // Collect current emitters from scene
    struct EmitterInfo {
        uint32_t id;
        uint32_t max_particles;
        ParticleBlendMode blend_mode;
    };
    std::vector<EmitterInfo> scene_emitters;
    for (const auto& obj : scene.objects()) {
        const auto* em = obj->components.get<EmitterComponent>();
        if (!em) continue;
        scene_emitters.push_back({obj->id, em->max_particles, em->blend_mode});
    }

    // Sort by blend mode (additive first) for render batching
    std::sort(scene_emitters.begin(), scene_emitters.end(), [](const auto& a, const auto& b) {
        return static_cast<uint8_t>(a.blend_mode) < static_cast<uint8_t>(b.blend_mode);
    });

    // Check if emitter set has changed
    bool changed = (scene_emitters.size() != emitters_.size());
    if (!changed) {
        for (size_t i = 0; i < emitters_.size(); ++i) {
            if (emitters_[i].object_id != scene_emitters[i].id ||
                emitters_[i].max_particles != scene_emitters[i].max_particles ||
                emitters_[i].blend_mode != scene_emitters[i].blend_mode) {
                changed = true;
                break;
            }
        }
    }

    if (!changed) return;

    // Rebuild emitter list with offsets
    emitters_.clear();
    emitters_.reserve(scene_emitters.size());
    uint32_t offset = 0;
    additive_count_ = 0;
    for (const auto& info : scene_emitters) {
        EmitterGpu gpu{};
        gpu.object_id = info.id;
        gpu.max_particles = info.max_particles;
        gpu.base_offset = offset;
        gpu.blend_mode = info.blend_mode;
        if (info.blend_mode == ParticleBlendMode::additive) ++additive_count_;
        emitters_.push_back(gpu);
        offset += info.max_particles;
    }

    uint32_t total = offset;
    uint32_t count = static_cast<uint32_t>(emitters_.size());

    bool needs_realloc = total > total_particle_capacity_ || count > emitter_capacity_;

    if (needs_realloc) {
        // Need larger buffers — wait for GPU, reallocate, then init
        vkDeviceWaitIdle(device_);
        uint32_t new_total = std::max(total, total_particle_capacity_ * 2);
        uint32_t new_count = std::max(count, emitter_capacity_ * 2);
        if (new_total == 0) new_total = total;
        if (new_count == 0) new_count = count;
        rebuild_mega_buffers(new_total, new_count);
    } else {
        // Capacity sufficient — just wait for previous frame's compute to finish
        vkQueueWaitIdle(queue_);
    }

    init_emitter_ranges();
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

    for (size_t i = 0; i < emitters_.size(); ++i) {
        auto& gpu = emitters_[i];
        auto* obj = scene.find(gpu.object_id);
        auto& data = emitter_data_cache_[i];
        std::memset(&data, 0, sizeof(data));

        auto* em_ptr = obj ? obj->components.get<EmitterComponent>() : nullptr;
        if (!em_ptr) continue;
        auto* spatial = obj->as_spatial();
        if (!spatial) continue;
        auto& em = *em_ptr;

        em.emit_accumulator += em.emission_rate * dt;
        uint32_t emit_count = static_cast<uint32_t>(em.emit_accumulator);
        em.emit_accumulator -= static_cast<float>(emit_count);
        if (emit_count > max_emit_count) max_emit_count = emit_count;

        data.emitter_position = glm::vec3(spatial->world_matrix_[3]);
        data.dt = dt;
        data.gravity = em.gravity;
        data.max_particles = gpu.max_particles;
        data.color_start = em.color_start;
        data.color_end = em.color_end;
        data.size_start = em.size_start;
        data.size_end = em.size_end;
        data.lifetime_min = em.lifetime_min;
        data.lifetime_max = em.lifetime_max;
        data.velocity_min = em.velocity_min;
        data.emit_count = emit_count;
        data.velocity_max = em.velocity_max;
        data.random_seed = frame_seed_ ^ (gpu.object_id * 2654435761u);
        data.base_offset = gpu.base_offset;
        data.counter_offset = static_cast<uint32_t>(i) * 2;
        data.dead_list_offset = gpu.base_offset;
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

    uint32_t emitter_count = static_cast<uint32_t>(emitters_.size());
    uint32_t alpha_count = emitter_count - additive_count_;

    // Bind global + particle SSBO once
    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, render_layout_,
                            0, 1, &ctx.global_descriptor_set, 0, nullptr);
    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, render_layout_,
                            1, 1, &render_set_, 0, nullptr);

    VkViewport vp{};
    vp.width = static_cast<float>(ctx.target->extent().width);
    vp.height = static_cast<float>(ctx.target->extent().height);
    vp.maxDepth = 1.0f;

    VkRect2D sc{};
    sc.extent = ctx.target->extent();

    constexpr uint32_t stride = sizeof(VkDrawIndirectCommand);

    // Additive emitters
    if (additive_count_ > 0) {
        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, render_additive_);
        vkCmdSetViewport(ctx.cmd, 0, 1, &vp);
        vkCmdSetScissor(ctx.cmd, 0, 1, &sc);
        vkCmdDrawIndirect(ctx.cmd, mega_indirect_buffer_->handle(), 0,
                          additive_count_, stride);
    }

    // Alpha emitters
    if (alpha_count > 0) {
        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, render_alpha_);
        vkCmdSetViewport(ctx.cmd, 0, 1, &vp);
        vkCmdSetScissor(ctx.cmd, 0, 1, &sc);
        vkCmdDrawIndirect(ctx.cmd, mega_indirect_buffer_->handle(),
                          additive_count_ * stride, alpha_count, stride);
    }
}

} // namespace fjell
