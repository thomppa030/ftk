#include "renderer/pass/scene_sdf_pass.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/gpu/vk_check.hpp"
#include "renderer/resources/model.hpp"
#include "renderer/resources/shader_utils.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>

#include <cstring>

namespace fjell {

void SceneSdfPass::create(Device& device, VkDescriptorSetLayout /*global_set_layout*/) {
    device_ = device.handle();
    physical_device_ = device.physical_device();

    create_volume_image();
    create_compute_pipeline();

    FJELL_GFX_DEBUG("SceneSdfPass created ({}³ R16_SFLOAT, {:.1f}MB)",
                    resolution_, float(resolution_ * resolution_ * resolution_ * 2) / (1024.0f * 1024.0f));
}

void SceneSdfPass::destroy() {
    auto safe = [&](auto& h, auto fn) {
        if (h != VK_NULL_HANDLE) { fn(device_, h, nullptr); h = VK_NULL_HANDLE; }
    };

    mesh_sdf_buffer_.reset();
    instance_buffer_.reset();

    safe(composite_pipeline_, vkDestroyPipeline);
    safe(composite_layout_, vkDestroyPipelineLayout);
    safe(composite_set_layout_, vkDestroyDescriptorSetLayout);
    safe(pool_, vkDestroyDescriptorPool);
    safe(sdf_sampler_, vkDestroySampler);
    safe(sdf_view_, vkDestroyImageView);
    if (sdf_image_ != VK_NULL_HANDLE) {
        vkDestroyImage(device_, sdf_image_, nullptr);
        sdf_image_ = VK_NULL_HANDLE;
    }
    if (sdf_memory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, sdf_memory_, nullptr);
        sdf_memory_ = VK_NULL_HANDLE;
    }
}

void SceneSdfPass::create_volume_image() {
    // 3D texture
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_3D;
    ci.format = SDF_FORMAT;
    ci.extent = {resolution_, resolution_, resolution_};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vk_check(vkCreateImage(device_, &ci, nullptr, &sdf_image_), "SDF create image");

    VkMemoryRequirements reqs;
    vkGetImageMemoryRequirements(device_, sdf_image_, &reqs);

    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(physical_device_, &mem_props);

    uint32_t mem_type = UINT32_MAX;
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((reqs.memoryTypeBits & (1u << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            mem_type = i;
            break;
        }
    }

    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = reqs.size;
    alloc.memoryTypeIndex = mem_type;
    vk_check(vkAllocateMemory(device_, &alloc, nullptr, &sdf_memory_), "SDF pass");
    vk_check(vkBindImageMemory(device_, sdf_image_, sdf_memory_, 0), "SDF pass");

    // Image view
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = sdf_image_;
    vci.viewType = VK_IMAGE_VIEW_TYPE_3D;
    vci.format = SDF_FORMAT;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk_check(vkCreateImageView(device_, &vci, nullptr, &sdf_view_), "SDF pass");

    // Sampler (trilinear, clamp to edge)
    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vk_check(vkCreateSampler(device_, &sci, nullptr, &sdf_sampler_), "SDF pass");
}

void SceneSdfPass::create_compute_pipeline() {
    // Descriptor set layout: binding 0 = storage image, 1 = mesh SDF SSBO, 2 = instance SSBO
    VkDescriptorSetLayoutBinding bindings[3]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_ci.bindingCount = 3;
    layout_ci.pBindings = bindings;
    vk_check(vkCreateDescriptorSetLayout(device_, &layout_ci, nullptr, &composite_set_layout_), "SDF pass");

    // Push constants: volume_min (vec3) + instance_count (uint) + volume_max (vec3) + resolution (uint) = 32 bytes
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push.size = 32;

    VkPipelineLayoutCreateInfo pipe_ci{};
    pipe_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipe_ci.setLayoutCount = 1;
    pipe_ci.pSetLayouts = &composite_set_layout_;
    pipe_ci.pushConstantRangeCount = 1;
    pipe_ci.pPushConstantRanges = &push;
    vk_check(vkCreatePipelineLayout(device_, &pipe_ci, nullptr, &composite_layout_), "SDF pass");

    // Pipeline
    auto spv = read_spirv(base_path::resolve("shaders/sdf_composite.comp.spv"));
    auto mod = create_shader_module(device_, spv);

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = mod;
    stage.pName = "main";

    VkComputePipelineCreateInfo comp_ci{};
    comp_ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    comp_ci.stage = stage;
    comp_ci.layout = composite_layout_;
    vk_check(vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &comp_ci, nullptr, &composite_pipeline_), "SDF pass");
    vkDestroyShaderModule(device_, mod, nullptr);

    // Descriptor pool
    VkDescriptorPoolSize pool_sizes[2]{};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    pool_sizes[0].descriptorCount = 1;
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_sizes[1].descriptorCount = 2;

    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.maxSets = 1;
    pool_ci.poolSizeCount = 2;
    pool_ci.pPoolSizes = pool_sizes;
    vk_check(vkCreateDescriptorPool(device_, &pool_ci, nullptr, &pool_), "SDF pass");

    // Allocate descriptor set
    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = pool_;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &composite_set_layout_;
    vk_check(vkAllocateDescriptorSets(device_, &alloc_info, &composite_set_), "SDF pass");

    // Write binding 0 (storage image)
    VkDescriptorImageInfo img_info{};
    img_info.imageView = sdf_view_;
    img_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = composite_set_;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.descriptorCount = 1;
    write.pImageInfo = &img_info;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    // Allocate mesh SDF buffer (4MB — room for ~60 meshes at 32³)
    mesh_sdf_buffer_ = std::make_unique<Buffer>(
        device_, physical_device_, 4 * 1024 * 1024,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    // Allocate instance buffer (enough for 256 instances)
    instance_buffer_ = std::make_unique<Buffer>(
        device_, physical_device_, 256 * sizeof(GpuSdfInstance),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    // Write bindings 1 and 2
    VkDescriptorBufferInfo mesh_buf_info{};
    mesh_buf_info.buffer = mesh_sdf_buffer_->handle();
    mesh_buf_info.range = mesh_sdf_buffer_->size();

    VkDescriptorBufferInfo inst_buf_info{};
    inst_buf_info.buffer = instance_buffer_->handle();
    inst_buf_info.range = instance_buffer_->size();

    VkWriteDescriptorSet buf_writes[2]{};
    buf_writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    buf_writes[0].dstSet = composite_set_;
    buf_writes[0].dstBinding = 1;
    buf_writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    buf_writes[0].descriptorCount = 1;
    buf_writes[0].pBufferInfo = &mesh_buf_info;

    buf_writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    buf_writes[1].dstSet = composite_set_;
    buf_writes[1].dstBinding = 2;
    buf_writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    buf_writes[1].descriptorCount = 1;
    buf_writes[1].pBufferInfo = &inst_buf_info;

    vkUpdateDescriptorSets(device_, 2, buf_writes, 0, nullptr);
}

bool SceneSdfPass::register_model_sdf(const Model& model, uint32_t model_id) {
    if (!model.has_sdf()) return false;
    if (model_sdf_entries_.count(model_id)) return true; // already registered

    uint32_t res = model.sdf_grid_resolution();
    uint32_t voxel_count = res * res * res;
    const auto& data = model.sdf_grid_data();
    if (data.size() != voxel_count) return false;

    // Check buffer capacity
    uint32_t bytes_needed = voxel_count * sizeof(int16_t);
    uint32_t current_bytes = mesh_sdf_offset_ * sizeof(int16_t);
    if (current_bytes + bytes_needed > mesh_sdf_buffer_->size()) {
        FJELL_GFX_WARN("SDF mesh buffer full, cannot register model {}", model_id);
        return false;
    }

    // Upload to SSBO
    auto* dst = static_cast<int16_t*>(mesh_sdf_buffer_->mapped()) + mesh_sdf_offset_;
    std::memcpy(dst, data.data(), bytes_needed);

    // Compute max_dist for denormalization
    glm::vec3 extent = model.sdf_volume_max() - model.sdf_volume_min();
    float max_dist = glm::length(extent);

    MeshSdfEntry entry{};
    entry.offset = mesh_sdf_offset_;
    entry.resolution = res;
    entry.sdf_min = model.sdf_volume_min();
    entry.sdf_max = model.sdf_volume_max();
    entry.max_dist = max_dist;
    model_sdf_entries_[model_id] = entry;

    mesh_sdf_offset_ += voxel_count;
    dirty_ = true;

    FJELL_GFX_DEBUG("Registered SDF for model {} ({}³, offset={})",
                    model_id, res, entry.offset);
    return true;
}

const SceneSdfPass::MeshSdfEntry* SceneSdfPass::model_entry(uint32_t model_id) const {
    auto it = model_sdf_entries_.find(model_id);
    return it != model_sdf_entries_.end() ? &it->second : nullptr;
}

void SceneSdfPass::set_bounds(const glm::vec3& min, const glm::vec3& max) {
    if (volume_min_ != min || volume_max_ != max) {
        volume_min_ = min;
        volume_max_ = max;
        dirty_ = true;
    }
}

void SceneSdfPass::composite(VkCommandBuffer cmd, const std::vector<GpuSdfInstance>& instances) {
    if (instances.empty()) {
        dirty_ = false;
        return;
    }

    // Upload instance data
    uint32_t inst_count = static_cast<uint32_t>(instances.size());
    uint32_t inst_bytes = inst_count * sizeof(GpuSdfInstance);
    if (inst_bytes > instance_buffer_->size()) {
        FJELL_GFX_WARN("SDF instance buffer too small ({} instances)", inst_count);
        inst_count = static_cast<uint32_t>(instance_buffer_->size() / sizeof(GpuSdfInstance));
    }
    std::memcpy(instance_buffer_->mapped(), instances.data(), inst_count * sizeof(GpuSdfInstance));

    // Transition SDF image to GENERAL for compute write
    VkImageMemoryBarrier2 pre_barrier{};
    pre_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    pre_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    pre_barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    pre_barrier.image = sdf_image_;
    pre_barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    pre_barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    pre_barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;

    VkDependencyInfo pre_dep{};
    pre_dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    pre_dep.imageMemoryBarrierCount = 1;
    pre_dep.pImageMemoryBarriers = &pre_barrier;
    vkCmdPipelineBarrier2(cmd, &pre_dep);

    // Bind and dispatch
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, composite_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, composite_layout_,
                            0, 1, &composite_set_, 0, nullptr);

    struct PushData {
        glm::vec3 volume_min;
        uint32_t instance_count;
        glm::vec3 volume_max;
        uint32_t resolution;
    } push{volume_min_, inst_count, volume_max_, resolution_};

    vkCmdPushConstants(cmd, composite_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);

    uint32_t groups = (resolution_ + 3) / 4;
    vkCmdDispatch(cmd, groups, groups, groups);

    // Transition to SHADER_READ_ONLY for sampling
    VkImageMemoryBarrier2 post_barrier{};
    post_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    post_barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    post_barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    post_barrier.image = sdf_image_;
    post_barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    post_barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    post_barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    post_barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    post_barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;

    VkDependencyInfo post_dep{};
    post_dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    post_dep.imageMemoryBarrierCount = 1;
    post_dep.pImageMemoryBarriers = &post_barrier;
    vkCmdPipelineBarrier2(cmd, &post_dep);

    dirty_ = false;
}

void SceneSdfPass::render_slice(VkCommandBuffer cmd, uint32_t axis, float slice_pos) {
    if (!slice_initialized_) {
        // Create slice output image (2D RGBA8, SLICE_SIZE x SLICE_SIZE)
        VkImageCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_R8G8B8A8_UNORM;
        ci.extent = {SLICE_SIZE, SLICE_SIZE, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        vk_check(vkCreateImage(device_, &ci, nullptr, &slice_image_), "SDF slice image");

        VkMemoryRequirements reqs;
        vkGetImageMemoryRequirements(device_, slice_image_, &reqs);
        VkPhysicalDeviceMemoryProperties mem_props;
        vkGetPhysicalDeviceMemoryProperties(physical_device_, &mem_props);
        uint32_t mem_type = UINT32_MAX;
        for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
            if ((reqs.memoryTypeBits & (1u << i)) &&
                (mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                mem_type = i; break;
            }
        }
        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = reqs.size;
        alloc.memoryTypeIndex = mem_type;
        vk_check(vkAllocateMemory(device_, &alloc, nullptr, &slice_memory_), "SDF slice mem");
        vk_check(vkBindImageMemory(device_, slice_image_, slice_memory_, 0), "SDF slice bind");

        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = slice_image_;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vk_check(vkCreateImageView(device_, &vci, nullptr, &slice_view_), "SDF slice view");

        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_NEAREST;
        sci.minFilter = VK_FILTER_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vk_check(vkCreateSampler(device_, &sci, nullptr, &slice_sampler_), "SDF slice sampler");

        // Slice compute pipeline: binding 0 = scene_sdf sampler3D, binding 1 = slice_output image2D
        VkDescriptorSetLayoutBinding slice_bindings[2]{};
        slice_bindings[0].binding = 0;
        slice_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        slice_bindings[0].descriptorCount = 1;
        slice_bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        slice_bindings[1].binding = 1;
        slice_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        slice_bindings[1].descriptorCount = 1;
        slice_bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo slice_layout_ci{};
        slice_layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        slice_layout_ci.bindingCount = 2;
        slice_layout_ci.pBindings = slice_bindings;
        vk_check(vkCreateDescriptorSetLayout(device_, &slice_layout_ci, nullptr, &slice_set_layout_), "SDF slice layout");

        VkPushConstantRange push{};
        push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push.size = 12; // uint axis + float slice_pos + uint resolution

        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &slice_set_layout_;
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &push;
        vk_check(vkCreatePipelineLayout(device_, &plci, nullptr, &slice_layout_), "SDF slice pipe layout");

        auto spv = read_spirv(base_path::resolve("shaders/sdf_slice.comp.spv"));
        auto mod = create_shader_module(device_, spv);
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = mod;
        stage.pName = "main";
        VkComputePipelineCreateInfo comp_ci{};
        comp_ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        comp_ci.stage = stage;
        comp_ci.layout = slice_layout_;
        vk_check(vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &comp_ci, nullptr, &slice_pipeline_), "SDF slice pipeline");
        vkDestroyShaderModule(device_, mod, nullptr);

        // Descriptor pool + set
        VkDescriptorPoolSize slice_pool_sizes[2]{};
        slice_pool_sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        slice_pool_sizes[0].descriptorCount = 1;
        slice_pool_sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        slice_pool_sizes[1].descriptorCount = 1;
        VkDescriptorPoolCreateInfo dpci{};
        dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.maxSets = 1;
        dpci.poolSizeCount = 2;
        dpci.pPoolSizes = slice_pool_sizes;
        vk_check(vkCreateDescriptorPool(device_, &dpci, nullptr, &slice_pool_), "SDF slice pool");

        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = slice_pool_;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &slice_set_layout_;
        vk_check(vkAllocateDescriptorSets(device_, &dsai, &slice_set_), "SDF slice set");

        // Write descriptors
        VkDescriptorImageInfo sdf_info{};
        sdf_info.sampler = sdf_sampler_;
        sdf_info.imageView = sdf_view_;
        sdf_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo slice_info{};
        slice_info.imageView = slice_view_;
        slice_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = slice_set_;
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].descriptorCount = 1;
        writes[0].pImageInfo = &sdf_info;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = slice_set_;
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo = &slice_info;
        vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);

        // Register with ImGui
        slice_imgui_set_ = ImGui_ImplVulkan_AddTexture(
            slice_sampler_, slice_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        slice_initialized_ = true;
    }

    // Transition slice image to GENERAL for compute write
    VkImageMemoryBarrier2 pre{};
    pre.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    pre.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    pre.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    pre.image = slice_image_;
    pre.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    pre.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    pre.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo pre_dep{};
    pre_dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    pre_dep.imageMemoryBarrierCount = 1;
    pre_dep.pImageMemoryBarriers = &pre;
    vkCmdPipelineBarrier2(cmd, &pre_dep);

    // Dispatch slice compute
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, slice_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, slice_layout_,
                            0, 1, &slice_set_, 0, nullptr);

    struct { uint32_t axis; float slice_pos; uint32_t resolution; } push_data{axis, slice_pos, SLICE_SIZE};
    vkCmdPushConstants(cmd, slice_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push_data), &push_data);

    uint32_t groups = (SLICE_SIZE + 7) / 8;
    vkCmdDispatch(cmd, groups, groups, 1);

    // Transition to SHADER_READ_ONLY for ImGui sampling
    VkImageMemoryBarrier2 post{};
    post.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    post.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    post.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    post.image = slice_image_;
    post.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    post.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    post.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    post.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    post.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    VkDependencyInfo post_dep{};
    post_dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    post_dep.imageMemoryBarrierCount = 1;
    post_dep.pImageMemoryBarriers = &post;
    vkCmdPipelineBarrier2(cmd, &post_dep);
}

} // namespace fjell
