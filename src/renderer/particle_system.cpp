#include "renderer/particle_system.hpp"
#include "renderer/device.hpp"
#include "renderer/offscreen_pass.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"
#include "scene/scene.hpp"

#include <cstring>
#include <fstream>
#include <stdexcept>

namespace fjell {

struct ParticleComputePush {
    glm::vec3 emitter_position;
    float dt;
    glm::vec3 gravity;
    uint32_t max_particles;
    glm::vec4 color_start;
    glm::vec4 color_end;
    float size_start;
    float size_end;
    float lifetime_min;
    float lifetime_max;
    glm::vec3 velocity_min;
    uint32_t emit_count;
    glm::vec3 velocity_max;
    uint32_t random_seed;
};
static_assert(sizeof(ParticleComputePush) == 112);

VkShaderModule ParticleSystem::load_shader(const std::string& path) {
    auto full_path = base_path::resolve(path);
    std::ifstream file(full_path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open shader: " + full_path);
    }
    auto size = static_cast<size_t>(file.tellg());
    std::vector<char> code(size);
    file.seekg(0);
    file.read(code.data(), static_cast<std::streamsize>(size));

    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule module;
    if (vkCreateShaderModule(device_, &ci, nullptr, &module) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shader module: " + path);
    }
    return module;
}

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

    // ── Compute descriptor set layout (particles, counters, dead list) ──

    VkDescriptorSetLayoutBinding compute_bindings[3]{};
    for (int i = 0; i < 3; ++i) {
        compute_bindings[i].binding = static_cast<uint32_t>(i);
        compute_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        compute_bindings[i].descriptorCount = 1;
        compute_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_ci.bindingCount = 3;
    layout_ci.pBindings = compute_bindings;
    if (vkCreateDescriptorSetLayout(device_, &layout_ci, nullptr, &compute_set_layout_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create particle compute set layout");
    }

    // ── Indirect descriptor set layout (counters, indirect draw) ──

    VkDescriptorSetLayoutBinding indirect_bindings[2]{};
    for (int i = 0; i < 2; ++i) {
        indirect_bindings[i].binding = static_cast<uint32_t>(i);
        indirect_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        indirect_bindings[i].descriptorCount = 1;
        indirect_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo indirect_layout_ci{};
    indirect_layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    indirect_layout_ci.bindingCount = 2;
    indirect_layout_ci.pBindings = indirect_bindings;
    if (vkCreateDescriptorSetLayout(device_, &indirect_layout_ci, nullptr, &indirect_set_layout_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create particle indirect set layout");
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

    // ── Indirect pipeline layout ──

    VkPipelineLayoutCreateInfo indirect_pipe_ci{};
    indirect_pipe_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    indirect_pipe_ci.setLayoutCount = 1;
    indirect_pipe_ci.pSetLayouts = &indirect_set_layout_;
    if (vkCreatePipelineLayout(device_, &indirect_pipe_ci, nullptr, &indirect_layout_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create particle indirect pipeline layout");
    }

    // ── Compute pipelines ──

    auto emit_mod = load_shader("shaders/particle_emit.comp.spv");
    auto sim_mod = load_shader("shaders/particle_sim.comp.spv");
    auto indirect_mod = load_shader("shaders/particle_indirect.comp.spv");

    auto make_compute = [&](VkShaderModule mod, VkPipelineLayout layout) {
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = mod;
        stage.pName = "main";

        VkComputePipelineCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ci.stage = stage;
        ci.layout = layout;

        VkPipeline pipeline;
        if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create particle compute pipeline");
        }
        return pipeline;
    };

    emit_pipeline_ = make_compute(emit_mod, compute_layout_);
    sim_pipeline_ = make_compute(sim_mod, compute_layout_);
    indirect_pipeline_ = make_compute(indirect_mod, indirect_layout_);

    vkDestroyShaderModule(device_, emit_mod, nullptr);
    vkDestroyShaderModule(device_, sim_mod, nullptr);
    vkDestroyShaderModule(device_, indirect_mod, nullptr);

    // ── Render descriptor set layout (particle SSBO for vertex shader) ──

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

    auto vert_mod = load_shader("shaders/particle.vert.spv");
    auto frag_mod = load_shader("shaders/particle.frag.spv");

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

    VkPipelineRenderingCreateInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_info.colorAttachmentCount = 1;
    rendering_info.pColorAttachmentFormats = &color_format_;
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

        VkPipelineColorBlendStateCreateInfo color_blending{};
        color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        color_blending.attachmentCount = 1;
        color_blending.pAttachments = &blend;

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

    // ── Descriptor pool ──

    constexpr uint32_t MAX_EMITTERS = 32;
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = MAX_EMITTERS * 6;

    VkDescriptorPoolCreateInfo desc_pool_ci{};
    desc_pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    desc_pool_ci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    desc_pool_ci.maxSets = MAX_EMITTERS * 3;
    desc_pool_ci.poolSizeCount = 1;
    desc_pool_ci.pPoolSizes = &pool_size;
    if (vkCreateDescriptorPool(device_, &desc_pool_ci, nullptr, &pool_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create particle descriptor pool");
    }

    FJELL_GFX_DEBUG("Particle system created");
}

void ParticleSystem::destroy() {
    for (auto& e : emitters_) destroy_emitter_gpu(e);
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
    safe_destroy(indirect_layout_, vkDestroyPipelineLayout);
    safe_destroy(indirect_set_layout_, vkDestroyDescriptorSetLayout);
    safe_destroy(sim_pipeline_, vkDestroyPipeline);
    safe_destroy(emit_pipeline_, vkDestroyPipeline);
    safe_destroy(compute_layout_, vkDestroyPipelineLayout);
    safe_destroy(compute_set_layout_, vkDestroyDescriptorSetLayout);
    safe_destroy(command_pool_, vkDestroyCommandPool);
}

void ParticleSystem::create_emitter_gpu(EmitterGpu& gpu, uint32_t max_particles) {
    gpu.max_particles = max_particles;

    VkDeviceSize particle_size = static_cast<VkDeviceSize>(max_particles) * 64;
    VkDeviceSize dead_list_size = static_cast<VkDeviceSize>(max_particles) * sizeof(uint32_t);
    VkDeviceSize counter_size = 2 * sizeof(uint32_t);  // alive_count + dead_count
    VkDeviceSize indirect_size = 4 * sizeof(uint32_t); // VkDrawIndirectCommand

    gpu.particle_buffer = std::make_unique<Buffer>(
        device_, physical_device_, particle_size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    gpu.counter_buffer = std::make_unique<Buffer>(
        device_, physical_device_, counter_size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    gpu.dead_list_buffer = std::make_unique<Buffer>(
        device_, physical_device_, dead_list_size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    gpu.indirect_buffer = std::make_unique<Buffer>(
        device_, physical_device_, indirect_size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    init_dead_list(gpu);

    // ── Allocate descriptor sets ──

    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = pool_;
    alloc.descriptorSetCount = 1;

    alloc.pSetLayouts = &compute_set_layout_;
    vkAllocateDescriptorSets(device_, &alloc, &gpu.compute_set);

    alloc.pSetLayouts = &indirect_set_layout_;
    vkAllocateDescriptorSets(device_, &alloc, &gpu.indirect_set);

    alloc.pSetLayouts = &render_set_layout_;
    vkAllocateDescriptorSets(device_, &alloc, &gpu.render_set);

    // ── Write descriptors ──

    VkDescriptorBufferInfo particle_info{gpu.particle_buffer->handle(), 0, gpu.particle_buffer->size()};
    VkDescriptorBufferInfo counter_info{gpu.counter_buffer->handle(), 0, gpu.counter_buffer->size()};
    VkDescriptorBufferInfo dead_list_info{gpu.dead_list_buffer->handle(), 0, gpu.dead_list_buffer->size()};
    VkDescriptorBufferInfo indirect_info{gpu.indirect_buffer->handle(), 0, gpu.indirect_buffer->size()};

    VkWriteDescriptorSet writes[6]{};
    for (auto& w : writes) w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;

    writes[0].dstSet = gpu.compute_set; writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].descriptorCount = 1; writes[0].pBufferInfo = &particle_info;

    writes[1].dstSet = gpu.compute_set; writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].descriptorCount = 1; writes[1].pBufferInfo = &counter_info;

    writes[2].dstSet = gpu.compute_set; writes[2].dstBinding = 2;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].descriptorCount = 1; writes[2].pBufferInfo = &dead_list_info;

    writes[3].dstSet = gpu.indirect_set; writes[3].dstBinding = 0;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[3].descriptorCount = 1; writes[3].pBufferInfo = &counter_info;

    writes[4].dstSet = gpu.indirect_set; writes[4].dstBinding = 1;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[4].descriptorCount = 1; writes[4].pBufferInfo = &indirect_info;

    writes[5].dstSet = gpu.render_set; writes[5].dstBinding = 0;
    writes[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[5].descriptorCount = 1; writes[5].pBufferInfo = &particle_info;

    vkUpdateDescriptorSets(device_, 6, writes, 0, nullptr);
    gpu.initialized = true;
}

void ParticleSystem::init_dead_list(EmitterGpu& gpu) {
    // All buffers are DEVICE_LOCAL — use staging for initialization

    // Dead list: [0, 1, 2, ..., N-1]
    VkDeviceSize dead_list_size = static_cast<VkDeviceSize>(gpu.max_particles) * sizeof(uint32_t);
    {
        Buffer staging(device_, physical_device_, dead_list_size,
                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        staging.map();
        auto* data = static_cast<uint32_t*>(staging.mapped());
        for (uint32_t i = 0; i < gpu.max_particles; ++i) data[i] = i;
        Buffer::copy(device_, command_pool_, queue_, staging, *gpu.dead_list_buffer, dead_list_size);
    }

    // Counters: alive=0, dead=N
    {
        Buffer staging(device_, physical_device_, gpu.counter_buffer->size(),
                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        staging.map();
        auto* data = static_cast<uint32_t*>(staging.mapped());
        data[0] = 0;
        data[1] = gpu.max_particles;
        Buffer::copy(device_, command_pool_, queue_, staging, *gpu.counter_buffer, gpu.counter_buffer->size());
    }

    // Indirect draw command: zeroed
    {
        Buffer staging(device_, physical_device_, gpu.indirect_buffer->size(),
                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        staging.map();
        auto* data = static_cast<uint32_t*>(staging.mapped());
        data[0] = 0; data[1] = 1; data[2] = 0; data[3] = 0;
        Buffer::copy(device_, command_pool_, queue_, staging, *gpu.indirect_buffer, gpu.indirect_buffer->size());
    }

    // Particles: zeroed
    VkDeviceSize particle_size = static_cast<VkDeviceSize>(gpu.max_particles) * 64;
    {
        Buffer staging(device_, physical_device_, particle_size,
                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        staging.map();
        std::memset(staging.mapped(), 0, particle_size);
        Buffer::copy(device_, command_pool_, queue_, staging, *gpu.particle_buffer, particle_size);
    }
}

void ParticleSystem::destroy_emitter_gpu(EmitterGpu& gpu) {
    if (!gpu.initialized) return;
    if (gpu.compute_set) vkFreeDescriptorSets(device_, pool_, 1, &gpu.compute_set);
    if (gpu.indirect_set) vkFreeDescriptorSets(device_, pool_, 1, &gpu.indirect_set);
    if (gpu.render_set) vkFreeDescriptorSets(device_, pool_, 1, &gpu.render_set);
    gpu.particle_buffer.reset();
    gpu.counter_buffer.reset();
    gpu.dead_list_buffer.reset();
    gpu.indirect_buffer.reset();
    gpu.initialized = false;
}

void ParticleSystem::sync_emitters(const Scene& scene) {
    std::unordered_map<uint32_t, const RenderObject*> active;
    for (const auto& obj : scene.objects()) {
        if (obj.emitter) active[obj.id] = &obj;
    }

    for (auto it = emitters_.begin(); it != emitters_.end();) {
        if (active.find(it->object_id) == active.end()) {
            vkDeviceWaitIdle(device_);
            destroy_emitter_gpu(*it);
            it = emitters_.erase(it);
        } else {
            ++it;
        }
    }

    for (const auto& [id, obj] : active) {
        bool found = false;
        for (const auto& e : emitters_) {
            if (e.object_id == id) { found = true; break; }
        }
        if (!found) {
            EmitterGpu gpu{};
            gpu.object_id = id;
            create_emitter_gpu(gpu, obj->emitter->max_particles);
            emitters_.push_back(std::move(gpu));
        }
    }
}

void ParticleSystem::dispatch_compute(VkCommandBuffer cmd, float dt, Scene& scene) {
    if (emitters_.empty()) return;
    ++frame_seed_;

    auto buffer_barrier = [&](VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                              VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
        VkMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        barrier.srcStageMask = src_stage;
        barrier.srcAccessMask = src_access;
        barrier.dstStageMask = dst_stage;
        barrier.dstAccessMask = dst_access;

        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);
    };

    for (auto& gpu : emitters_) {
        if (!gpu.initialized) continue;

        auto* obj = scene.find(gpu.object_id);
        if (!obj || !obj->emitter) continue;
        auto& em = *obj->emitter;

        // Compute how many particles to emit this frame
        em.emit_accumulator += em.emission_rate * dt;
        uint32_t emit_count = static_cast<uint32_t>(em.emit_accumulator);
        em.emit_accumulator -= static_cast<float>(emit_count);

        // Build push constants
        glm::vec3 pos = glm::vec3(obj->world_matrix_[3]);
        ParticleComputePush push{};
        push.emitter_position = pos;
        push.dt = dt;
        push.gravity = em.gravity;
        push.max_particles = gpu.max_particles;
        push.color_start = em.color_start;
        push.color_end = em.color_end;
        push.size_start = em.size_start;
        push.size_end = em.size_end;
        push.lifetime_min = em.lifetime_min;
        push.lifetime_max = em.lifetime_max;
        push.velocity_min = em.velocity_min;
        push.emit_count = emit_count;
        push.velocity_max = em.velocity_max;
        push.random_seed = frame_seed_ ^ (gpu.object_id * 2654435761u);

        // ── Emit dispatch ──
        if (emit_count > 0) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, emit_pipeline_);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compute_layout_,
                                    0, 1, &gpu.compute_set, 0, nullptr);
            vkCmdPushConstants(cmd, compute_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                               0, sizeof(push), &push);
            vkCmdDispatch(cmd, (emit_count + 255) / 256, 1, 1);

            buffer_barrier(
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        }

        // ── Simulate dispatch ──
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sim_pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compute_layout_,
                                0, 1, &gpu.compute_set, 0, nullptr);
        vkCmdPushConstants(cmd, compute_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(push), &push);
        vkCmdDispatch(cmd, (gpu.max_particles + 255) / 256, 1, 1);

        buffer_barrier(
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

        // ── Indirect draw command dispatch ──
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, indirect_pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, indirect_layout_,
                                0, 1, &gpu.indirect_set, 0, nullptr);
        vkCmdDispatch(cmd, 1, 1, 1);
    }
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
    if (emitters_.empty()) return;

    // Bind global descriptor set at set 0
    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, render_layout_,
                            0, 1, &ctx.global_descriptor_set, 0, nullptr);

    VkPipeline bound_pipeline = VK_NULL_HANDLE;

    for (const auto& gpu : emitters_) {
        if (!gpu.initialized) continue;

        // Determine blend mode from scene
        VkPipeline target_pipeline = render_additive_;
        if (ctx.scene) {
            const auto* obj = ctx.scene->find(gpu.object_id);
            if (obj && obj->emitter && obj->emitter->blend_mode == ParticleBlendMode::alpha) {
                target_pipeline = render_alpha_;
            }
        }

        if (target_pipeline != bound_pipeline) {
            vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, target_pipeline);
            bound_pipeline = target_pipeline;

            // Re-set viewport/scissor after pipeline bind
            VkViewport vp{};
            vp.width = static_cast<float>(ctx.target->extent().width);
            vp.height = static_cast<float>(ctx.target->extent().height);
            vp.maxDepth = 1.0f;
            vkCmdSetViewport(ctx.cmd, 0, 1, &vp);

            VkRect2D sc{};
            sc.extent = ctx.target->extent();
            vkCmdSetScissor(ctx.cmd, 0, 1, &sc);
        }

        // Bind particle SSBO at set 1
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, render_layout_,
                                1, 1, &gpu.render_set, 0, nullptr);

        // Draw all particle slots — dead particles produce degenerate triangles
        vkCmdDraw(ctx.cmd, gpu.max_particles * 6, 1, 0, 0);
    }
}

} // namespace fjell
