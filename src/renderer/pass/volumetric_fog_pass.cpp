#include "renderer/pass/volumetric_fog_pass.hpp"
#include "renderer/buffer.hpp"
#include "renderer/shader_utils.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace fjell {

struct FogPushConstants {
    glm::uvec4 volume_dims;   // xyz = froxel grid, w = frame_index
    float near_plane;
    float far_plane;
    float density;
    float height;
    float height_falloff;
    float scattering;
    float anisotropy;
    float temporal_blend;
    uint32_t viewport_width;
    uint32_t viewport_height;
    float fog_color_r;
    float fog_color_g;
    float fog_color_b;
    float jitter_x;           // precomputed Halton jitter
    float jitter_y;
    float jitter_z;
    float padding;
};

// Halton sequence (computed on CPU, passed via push constants)
static float halton(int index, int base) {
    float result = 0.0f;
    float f = 1.0f / static_cast<float>(base);
    int i = index;
    while (i > 0) {
        result += f * static_cast<float>(i % base);
        i /= base;
        f /= static_cast<float>(base);
    }
    return result;
}

// ── Helpers ──────────────────────────────────────────────────────────────────

static VkPipeline create_compute_pipeline(VkDevice device, VkPipelineLayout layout,
                                           const std::string& shader_path) {
    auto code = read_spirv(base_path::resolve(shader_path));
    auto module = create_shader_module(device, code);

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module;
    stage.pName = "main";

    VkComputePipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage = stage;
    ci.layout = layout;

    VkPipeline pipeline;
    if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci,
                                  nullptr, &pipeline) != VK_SUCCESS) {
        vkDestroyShaderModule(device, module, nullptr);
        throw std::runtime_error("Failed to create volumetric fog compute pipeline");
    }
    vkDestroyShaderModule(device, module, nullptr);
    return pipeline;
}

static VkPipelineLayout create_pipeline_layout(
        VkDevice device,
        const VkDescriptorSetLayout* set_layouts, uint32_t set_count,
        uint32_t push_size) {
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset = 0;
    push_range.size = push_size;

    VkPipelineLayoutCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    ci.setLayoutCount = set_count;
    ci.pSetLayouts = set_layouts;
    ci.pushConstantRangeCount = 1;
    ci.pPushConstantRanges = &push_range;

    VkPipelineLayout layout;
    if (vkCreatePipelineLayout(device, &ci, nullptr, &layout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create volumetric fog pipeline layout");
    }
    return layout;
}

// ── VolumeImage helpers ─────────────────────────────────────────────────────

VolumetricFogPass::VolumeImage VolumetricFogPass::create_volume_image(
        uint32_t w, uint32_t h, uint32_t d) {
    VolumeImage img{};

    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_3D;
    ci.format = VOLUME_FORMAT;
    ci.extent = {w, h, d};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(device_, &ci, nullptr, &img.image) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create fog volume image");
    }

    VkMemoryRequirements reqs;
    vkGetImageMemoryRequirements(device_, img.image, &reqs);

    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = reqs.size;
    alloc.memoryTypeIndex = Buffer::find_memory_type(
        physical_device_, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(device_, &alloc, nullptr, &img.memory) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate fog volume memory");
    }
    vkBindImageMemory(device_, img.image, img.memory, 0);

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_3D;
    vi.format = VOLUME_FORMAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    if (vkCreateImageView(device_, &vi, nullptr, &img.view) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create fog volume image view");
    }

    return img;
}

void VolumetricFogPass::destroy_volume_image(VolumeImage& img) {
    if (img.view) vkDestroyImageView(device_, img.view, nullptr);
    if (img.image) vkDestroyImage(device_, img.image, nullptr);
    if (img.memory) vkFreeMemory(device_, img.memory, nullptr);
    img = {};
}

// ── Create / Destroy ────────────────────────────────────────────────────────

void VolumetricFogPass::create(VkDevice device, VkPhysicalDevice physical_device,
                                VkDescriptorSetLayout global_set_layout) {
    device_ = device;
    physical_device_ = physical_device;

    // Linear sampler for trilinear 3D texture sampling
    VkSamplerCreateInfo sampler_ci{};
    sampler_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_ci.magFilter = VK_FILTER_LINEAR;
    sampler_ci.minFilter = VK_FILTER_LINEAR;
    sampler_ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device_, &sampler_ci, nullptr, &linear_sampler_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create fog sampler");
    }

    // ── Inject set layout: binding 0 = storage image (scatter write) ──
    {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ci.bindingCount = 1;
        ci.pBindings = &binding;
        vkCreateDescriptorSetLayout(device_, &ci, nullptr, &inject_set_layout_);

        VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2};
        VkDescriptorPoolCreateInfo pool_ci{};
        pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_ci.maxSets = 2;
        pool_ci.poolSizeCount = 1;
        pool_ci.pPoolSizes = &pool_size;
        vkCreateDescriptorPool(device_, &pool_ci, nullptr, &inject_pool_);

        std::array<VkDescriptorSetLayout, 2> layouts2{inject_set_layout_, inject_set_layout_};
        VkDescriptorSetAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool = inject_pool_;
        alloc.descriptorSetCount = 2;
        alloc.pSetLayouts = layouts2.data();
        vkAllocateDescriptorSets(device_, &alloc, inject_sets_);

        std::array layouts{global_set_layout, inject_set_layout_};
        inject_pipeline_layout_ = create_pipeline_layout(
            device_, layouts.data(), static_cast<uint32_t>(layouts.size()),
            sizeof(FogPushConstants));
        inject_pipeline_ = create_compute_pipeline(
            device_, inject_pipeline_layout_, "shaders/fog_inject.comp.spv");
    }

    // ── Temporal set layout: binding 0 = storage image (current rw), binding 1 = sampler3D (prev) ──
    {
        std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ci.bindingCount = static_cast<uint32_t>(bindings.size());
        ci.pBindings = bindings.data();
        vkCreateDescriptorSetLayout(device_, &ci, nullptr, &temporal_set_layout_);

        std::array<VkDescriptorPoolSize, 2> pool_sizes{};
        pool_sizes[0] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2};
        pool_sizes[1] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
        VkDescriptorPoolCreateInfo pool_ci{};
        pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_ci.maxSets = 2;
        pool_ci.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
        pool_ci.pPoolSizes = pool_sizes.data();
        vkCreateDescriptorPool(device_, &pool_ci, nullptr, &temporal_pool_);

        std::array<VkDescriptorSetLayout, 2> layouts2{temporal_set_layout_, temporal_set_layout_};
        VkDescriptorSetAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool = temporal_pool_;
        alloc.descriptorSetCount = 2;
        alloc.pSetLayouts = layouts2.data();
        vkAllocateDescriptorSets(device_, &alloc, temporal_sets_);

        std::array layouts{global_set_layout, temporal_set_layout_};
        temporal_pipeline_layout_ = create_pipeline_layout(
            device_, layouts.data(), static_cast<uint32_t>(layouts.size()),
            sizeof(FogPushConstants));
        temporal_pipeline_ = create_compute_pipeline(
            device_, temporal_pipeline_layout_, "shaders/fog_temporal.comp.spv");
    }

    // ── Integrate set layout: binding 0 = sampler3D (scatter), binding 1 = storage image (integrated) ──
    {
        std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ci.bindingCount = static_cast<uint32_t>(bindings.size());
        ci.pBindings = bindings.data();
        vkCreateDescriptorSetLayout(device_, &ci, nullptr, &integrate_set_layout_);

        std::array<VkDescriptorPoolSize, 2> pool_sizes{};
        pool_sizes[0] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
        pool_sizes[1] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2};
        VkDescriptorPoolCreateInfo pool_ci{};
        pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_ci.maxSets = 2;
        pool_ci.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
        pool_ci.pPoolSizes = pool_sizes.data();
        vkCreateDescriptorPool(device_, &pool_ci, nullptr, &integrate_pool_);

        std::array<VkDescriptorSetLayout, 2> layouts2{integrate_set_layout_, integrate_set_layout_};
        VkDescriptorSetAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool = integrate_pool_;
        alloc.descriptorSetCount = 2;
        alloc.pSetLayouts = layouts2.data();
        vkAllocateDescriptorSets(device_, &alloc, integrate_sets_);

        integrate_pipeline_layout_ = create_pipeline_layout(
            device_, &integrate_set_layout_, 1, sizeof(FogPushConstants));
        integrate_pipeline_ = create_compute_pipeline(
            device_, integrate_pipeline_layout_, "shaders/fog_integrate.comp.spv");
    }

    // ── Composite set layout: binding 0 = sampler3D, binding 1 = sampler2D (depth), binding 2 = storage image (color) ──
    {
        std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        bindings[2].binding = 2;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ci.bindingCount = static_cast<uint32_t>(bindings.size());
        ci.pBindings = bindings.data();
        vkCreateDescriptorSetLayout(device_, &ci, nullptr, &composite_set_layout_);

        std::array<VkDescriptorPoolSize, 2> pool_sizes{};
        pool_sizes[0] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * MAX_FRAMES_IN_FLIGHT};
        pool_sizes[1] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, MAX_FRAMES_IN_FLIGHT};
        VkDescriptorPoolCreateInfo pool_ci{};
        pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_ci.maxSets = MAX_FRAMES_IN_FLIGHT;
        pool_ci.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
        pool_ci.pPoolSizes = pool_sizes.data();
        vkCreateDescriptorPool(device_, &pool_ci, nullptr, &composite_pool_);

        std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> layouts3{};
        layouts3.fill(composite_set_layout_);
        VkDescriptorSetAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool = composite_pool_;
        alloc.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
        alloc.pSetLayouts = layouts3.data();
        vkAllocateDescriptorSets(device_, &alloc, composite_sets_);

        std::array layouts{global_set_layout, composite_set_layout_};
        composite_pipeline_layout_ = create_pipeline_layout(
            device_, layouts.data(), static_cast<uint32_t>(layouts.size()),
            sizeof(FogPushConstants));
        composite_pipeline_ = create_compute_pipeline(
            device_, composite_pipeline_layout_, "shaders/fog_composite.comp.spv");
    }

    // Create initial 1x1x1 volumes so descriptors are valid
    volume_extent_ = {1, 1, 1};
    create_volumes();

    FJELL_GFX_DEBUG("Volumetric fog pass created");
}

void VolumetricFogPass::destroy() {
    if (!device_) return;

    destroy_volumes();

    auto destroy_pass = [&](VkPipeline& p, VkPipelineLayout& l,
                            VkDescriptorSetLayout& sl, VkDescriptorPool& pool) {
        if (p) { vkDestroyPipeline(device_, p, nullptr); p = VK_NULL_HANDLE; }
        if (l) { vkDestroyPipelineLayout(device_, l, nullptr); l = VK_NULL_HANDLE; }
        if (sl) { vkDestroyDescriptorSetLayout(device_, sl, nullptr); sl = VK_NULL_HANDLE; }
        if (pool) { vkDestroyDescriptorPool(device_, pool, nullptr); pool = VK_NULL_HANDLE; }
    };

    destroy_pass(inject_pipeline_, inject_pipeline_layout_, inject_set_layout_, inject_pool_);
    destroy_pass(temporal_pipeline_, temporal_pipeline_layout_, temporal_set_layout_, temporal_pool_);
    destroy_pass(integrate_pipeline_, integrate_pipeline_layout_, integrate_set_layout_, integrate_pool_);
    destroy_pass(composite_pipeline_, composite_pipeline_layout_, composite_set_layout_, composite_pool_);

    if (linear_sampler_) { vkDestroySampler(device_, linear_sampler_, nullptr); linear_sampler_ = VK_NULL_HANDLE; }

    device_ = VK_NULL_HANDLE;
}

void VolumetricFogPass::create_volumes() {
    uint32_t w = volume_extent_.width;
    uint32_t h = volume_extent_.height;
    uint32_t d = volume_extent_.depth;

    scatter_volumes_[0] = create_volume_image(w, h, d);
    scatter_volumes_[1] = create_volume_image(w, h, d);
    integrated_volume_ = create_volume_image(w, h, d);

    volumes_need_init_ = true;
    ping_pong_ = 0;

    // Pre-fill both ping-pong descriptor sets so we never update during dispatch
    for (uint32_t i = 0; i < 2; ++i) {
        update_inject_descriptors(i);
        update_temporal_descriptors(i);
        update_integrate_descriptors(i);
    }
}

void VolumetricFogPass::destroy_volumes() {
    destroy_volume_image(scatter_volumes_[0]);
    destroy_volume_image(scatter_volumes_[1]);
    destroy_volume_image(integrated_volume_);
    imgui_descriptor_ = VK_NULL_HANDLE;
}

void VolumetricFogPass::resize(uint32_t width, uint32_t height) {
    uint32_t vol_w = std::max(1u, width / SCREEN_DIVISOR);
    uint32_t vol_h = std::max(1u, height / SCREEN_DIVISOR);

    if (vol_w == volume_extent_.width && vol_h == volume_extent_.height) return;

    // Skip resize if volumes already exist and change is tiny (< 4 texels)
    // This prevents thrashing during interactive viewport dragging
    if (volume_extent_.width > 1) {
        int dw = static_cast<int>(vol_w) - static_cast<int>(volume_extent_.width);
        int dh = static_cast<int>(vol_h) - static_cast<int>(volume_extent_.height);
        if (std::abs(dw) < 4 && std::abs(dh) < 4) return;
    }

    vkDeviceWaitIdle(device_);
    destroy_volumes();

    volume_extent_ = {vol_w, vol_h, DEPTH_SLICES};
    create_volumes();

    invalidate_imgui_texture();
}

// ── Descriptor updates ──────────────────────────────────────────────────────

void VolumetricFogPass::update_inject_descriptors(uint32_t idx) {
    VkDescriptorImageInfo storage_info{};
    storage_info.imageView = scatter_volumes_[idx].view;
    storage_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = inject_sets_[idx];
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.descriptorCount = 1;
    write.pImageInfo = &storage_info;

    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
}

void VolumetricFogPass::update_temporal_descriptors(uint32_t idx) {
    // When ping_pong==idx: writes to scatter[idx], reads history from scatter[1-idx]
    uint32_t other = 1 - idx;

    VkDescriptorImageInfo storage_info{};
    storage_info.imageView = scatter_volumes_[idx].view;
    storage_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo sampled_info{};
    sampled_info.sampler = linear_sampler_;
    sampled_info.imageView = scatter_volumes_[other].view;
    sampled_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    std::array<VkWriteDescriptorSet, 2> writes{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = temporal_sets_[idx];
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo = &storage_info;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = temporal_sets_[idx];
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo = &sampled_info;

    vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void VolumetricFogPass::update_integrate_descriptors(uint32_t idx) {
    VkDescriptorImageInfo sampled_info{};
    sampled_info.sampler = linear_sampler_;
    sampled_info.imageView = scatter_volumes_[idx].view;
    sampled_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo storage_info{};
    storage_info.imageView = integrated_volume_.view;
    storage_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    std::array<VkWriteDescriptorSet, 2> writes{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = integrate_sets_[idx];
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo = &sampled_info;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = integrate_sets_[idx];
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo = &storage_info;

    vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void VolumetricFogPass::update_composite_descriptors(uint32_t frame,
                                                       VkImageView depth_view,
                                                       VkImageView color_view) {
    VkDescriptorImageInfo fog_info{};
    fog_info.sampler = linear_sampler_;
    fog_info.imageView = integrated_volume_.view;
    fog_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo depth_info{};
    depth_info.sampler = linear_sampler_;
    depth_info.imageView = depth_view;
    depth_info.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo color_info{};
    color_info.imageView = color_view;
    color_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    std::array<VkWriteDescriptorSet, 3> writes{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = composite_sets_[frame];
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo = &fog_info;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = composite_sets_[frame];
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo = &depth_info;

    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = composite_sets_[frame];
    writes[2].dstBinding = 2;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[2].descriptorCount = 1;
    writes[2].pImageInfo = &color_info;

    vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

// ── Dispatch ────────────────────────────────────────────────────────────────

static void batch_barriers(VkCommandBuffer cmd,
                            const VkImageMemoryBarrier2* barriers, uint32_t count) {
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = count;
    dep.pImageMemoryBarriers = barriers;
    vkCmdPipelineBarrier2(cmd, &dep);
}

static VkImageMemoryBarrier2 make_barrier(VkImage image,
                                            VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                                            VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access,
                                            VkImageLayout old_layout, VkImageLayout new_layout) {
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.oldLayout = old_layout;
    b.newLayout = new_layout;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return b;
}

void VolumetricFogPass::dispatch(VkCommandBuffer cmd, VkDescriptorSet global_set,
                                  VkImageView depth_view, VkImageView color_view,
                                  uint32_t width, uint32_t height,
                                  uint32_t frame_index) {
    if (!enabled_) return;
    if (scatter_volumes_[0].view == VK_NULL_HANDLE) return;

    // Ping-pong: current frame writes to [ping_pong_], reads history from [1-ping_pong_]
    uint32_t cur = ping_pong_;
    uint32_t prev = 1 - cur;

    // Always update this frame's composite set (each frame has its own, safe to update)
    update_composite_descriptors(frame_index, depth_view, color_view);

    uint32_t vol_w = volume_extent_.width;
    uint32_t vol_h = volume_extent_.height;
    uint32_t vol_d = volume_extent_.depth;

    // Precompute Halton jitter on CPU
    int frame_idx = static_cast<int>(frame_counter_ % 16) + 1;

    FogPushConstants push{};
    push.volume_dims = glm::uvec4(vol_w, vol_h, vol_d, frame_counter_);
    push.near_plane = 0.1f;
    push.far_plane = max_distance_;
    push.density = density_;
    push.height = height_;
    push.height_falloff = height_falloff_;
    push.scattering = scattering_;
    push.anisotropy = anisotropy_;
    push.temporal_blend = temporal_blend_;
    push.viewport_width = width;
    push.viewport_height = height;
    push.fog_color_r = fog_color_.r;
    push.fog_color_g = fog_color_.g;
    push.fog_color_b = fog_color_.b;
    push.jitter_x = halton(frame_idx, 3) - 0.5f;
    push.jitter_y = halton(frame_idx, 5) - 0.5f;
    push.jitter_z = halton(frame_idx, 2) - 0.5f;

    uint32_t groups_x = (vol_w + TILE_SIZE - 1) / TILE_SIZE;
    uint32_t groups_y = (vol_h + TILE_SIZE - 1) / TILE_SIZE;

    // ── 1. Initialize volumes on first frame / after resize ──────────
    if (volumes_need_init_) {
        std::array<VkImageMemoryBarrier2, 3> init{};
        init[0] = make_barrier(scatter_volumes_[0].image,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        init[1] = make_barrier(scatter_volumes_[1].image,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        init[2] = make_barrier(integrated_volume_.image,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        batch_barriers(cmd, init.data(), static_cast<uint32_t>(init.size()));

        volumes_need_init_ = false;
    }

    // ── 2. Inject: density + light scattering → scatter_volumes_[cur] ──
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, inject_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, inject_pipeline_layout_,
                            0, 1, &global_set, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, inject_pipeline_layout_,
                            1, 1, &inject_sets_[cur], 0, nullptr);
    vkCmdPushConstants(cmd, inject_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);
    vkCmdDispatch(cmd, groups_x, groups_y, vol_d);

    // ── 3. Barrier: inject write → temporal read-write ───────────────
    {
        auto b = make_barrier(scatter_volumes_[cur].image,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        batch_barriers(cmd, &b, 1);
    }

    // ── 4. Temporal: blend scatter_volumes_[cur] with scatter_volumes_[prev] ──
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, temporal_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, temporal_pipeline_layout_,
                            0, 1, &global_set, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, temporal_pipeline_layout_,
                            1, 1, &temporal_sets_[cur], 0, nullptr);
    vkCmdPushConstants(cmd, temporal_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);
    vkCmdDispatch(cmd, groups_x, groups_y, vol_d);

    // ── 5. Barriers: scatter[cur] → readable for integrate, scatter[prev] stays readable ──
    {
        auto b = make_barrier(scatter_volumes_[cur].image,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        batch_barriers(cmd, &b, 1);
    }

    // ── 6. Integrate: front-to-back ray march ────────────────────────
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, integrate_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, integrate_pipeline_layout_,
                            0, 1, &integrate_sets_[cur], 0, nullptr);
    vkCmdPushConstants(cmd, integrate_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);
    vkCmdDispatch(cmd, groups_x, groups_y, 1);

    // ── 7. Barrier: integrated → readable for composite ──────────────
    {
        auto b = make_barrier(integrated_volume_.image,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        batch_barriers(cmd, &b, 1);
    }

    // ── 8. Composite: apply fog to scene color ───────────────────────
    uint32_t screen_groups_x = (width + TILE_SIZE - 1) / TILE_SIZE;
    uint32_t screen_groups_y = (height + TILE_SIZE - 1) / TILE_SIZE;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, composite_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, composite_pipeline_layout_,
                            0, 1, &global_set, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, composite_pipeline_layout_,
                            1, 1, &composite_sets_[frame_index], 0, nullptr);
    vkCmdPushConstants(cmd, composite_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);
    vkCmdDispatch(cmd, screen_groups_x, screen_groups_y, 1);

    // ── 9. Restore layouts for next frame ────────────────────────────
    // scatter[cur]: SHADER_READ_ONLY → stays there (becomes prev next frame)
    // integrated: SHADER_READ_ONLY → GENERAL (for next frame's write)
    // scatter[prev]: SHADER_READ_ONLY → GENERAL (becomes cur next frame)
    {
        std::array<VkImageMemoryBarrier2, 2> restore{};
        restore[0] = make_barrier(integrated_volume_.image,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        restore[1] = make_barrier(scatter_volumes_[prev].image,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        batch_barriers(cmd, restore.data(), static_cast<uint32_t>(restore.size()));
    }

    // Flip ping-pong for next frame
    ping_pong_ = prev;
    ++frame_counter_;
}

// ── ImGui texture ───────────────────────────────────────────────────────────

VkDescriptorSet VolumetricFogPass::imgui_texture() {
    if (!imgui_descriptor_ && integrated_volume_.view) {
        imgui_descriptor_ = ImGui_ImplVulkan_AddTexture(
            linear_sampler_, integrated_volume_.view,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return imgui_descriptor_;
}

void VolumetricFogPass::invalidate_imgui_texture() {
    imgui_descriptor_ = VK_NULL_HANDLE;
}

} // namespace fjell
