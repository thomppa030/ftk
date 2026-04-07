#include "renderer/pass/sky_cubemap_pass.hpp"
#include "renderer/gpu/vma_image.hpp"
#include "renderer/resources/shader_utils.hpp"
#include "renderer/gpu/vk_check.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace fjell {

void SkyCubemapPass::create(VkDevice device, VmaAllocator allocator,
                            VkCommandPool command_pool, VkQueue queue,
                            VkPipelineCache pipeline_cache) {
    device_ = device;
    vk_pipeline_cache_ = pipeline_cache;
    allocator_ = allocator;
    command_pool_ = command_pool;
    queue_ = queue;

    create_cubemap();
    create_cubemap_pipeline();
    create_brdf_lut();
    create_brdf_pipeline();
    generate_brdf_lut();

    FJELL_GFX_DEBUG("Sky cubemap pass created");
}

void SkyCubemapPass::destroy() {
    if (device_ == VK_NULL_HANDLE) return;

    // BRDF compute
    if (brdf_pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device_, brdf_pipeline_, nullptr);
    if (brdf_pipeline_layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, brdf_pipeline_layout_, nullptr);
    if (brdf_pool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(device_, brdf_pool_, nullptr);
    if (brdf_set_layout_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device_, brdf_set_layout_, nullptr);

    // BRDF LUT image
    if (brdf_lut_sampler_ != VK_NULL_HANDLE) vkDestroySampler(device_, brdf_lut_sampler_, nullptr);
    if (brdf_lut_view_ != VK_NULL_HANDLE) vkDestroyImageView(device_, brdf_lut_view_, nullptr);
    destroy_vma_image(allocator_, brdf_lut_image_, brdf_lut_memory_);

    // Cubemap compute
    if (cubemap_pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device_, cubemap_pipeline_, nullptr);
    if (cubemap_pipeline_layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, cubemap_pipeline_layout_, nullptr);
    if (cubemap_pool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(device_, cubemap_pool_, nullptr);
    if (cubemap_set_layout_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device_, cubemap_set_layout_, nullptr);

    // Cubemap image
    if (cubemap_sampler_ != VK_NULL_HANDLE) vkDestroySampler(device_, cubemap_sampler_, nullptr);
    if (cubemap_storage_view_ != VK_NULL_HANDLE) vkDestroyImageView(device_, cubemap_storage_view_, nullptr);
    if (cubemap_view_ != VK_NULL_HANDLE) vkDestroyImageView(device_, cubemap_view_, nullptr);
    destroy_vma_image(allocator_, cubemap_image_, cubemap_memory_);

    device_ = VK_NULL_HANDLE;
}

void SkyCubemapPass::set_colors(const glm::vec3& top, const glm::vec3& bottom) {
    if (top_color_ != top || bottom_color_ != bottom) {
        top_color_ = top;
        bottom_color_ = bottom;
        dirty_ = true;
    }
}

void SkyCubemapPass::generate(VkCommandBuffer cmd) {
    if (!dirty_) return;

    // Transition cubemap mip 0 to GENERAL for compute write
    {
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = first_generate_
            ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT
            : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.srcAccessMask = first_generate_
            ? 0
            : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        barrier.oldLayout = first_generate_
            ? VK_IMAGE_LAYOUT_UNDEFINED
            : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = cubemap_image_;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = MIP_LEVELS;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 6;

        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    // Dispatch cubemap compute shader
    struct PushConstants {
        glm::vec4 top_color;
        glm::vec4 bottom_color;
        uint32_t face_size;
    } push{
        glm::vec4(top_color_, 1.0f),
        glm::vec4(bottom_color_, 1.0f),
        CUBEMAP_SIZE
    };

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cubemap_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            cubemap_pipeline_layout_, 0, 1, &cubemap_set_, 0, nullptr);
    vkCmdPushConstants(cmd, cubemap_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);

    uint32_t groups_xy = (CUBEMAP_SIZE + 7) / 8;
    vkCmdDispatch(cmd, groups_xy, groups_xy, 6);

    // Generate mips via vkCmdBlitImage
    generate_mips(cmd);

    dirty_ = false;
    first_generate_ = false;
}

void SkyCubemapPass::generate_mips(VkCommandBuffer cmd) {
    // After compute: mip 0 is in GENERAL layout. Transition to TRANSFER_SRC.
    {
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = cubemap_image_;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 6;

        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    int32_t mip_width = static_cast<int32_t>(CUBEMAP_SIZE);
    int32_t mip_height = static_cast<int32_t>(CUBEMAP_SIZE);

    for (uint32_t mip = 1; mip < MIP_LEVELS; ++mip) {
        int32_t next_width = std::max(mip_width / 2, 1);
        int32_t next_height = std::max(mip_height / 2, 1);

        // Transition dst mip to TRANSFER_DST
        {
            VkImageMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
            barrier.srcAccessMask = 0;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL; // was set to GENERAL earlier
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = cubemap_image_;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.baseMipLevel = mip;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.baseArrayLayer = 0;
            barrier.subresourceRange.layerCount = 6;

            VkDependencyInfo dep{};
            dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(cmd, &dep);
        }

        // Blit from previous mip to this mip
        VkImageBlit2 blit{};
        blit.sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2;
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel = mip - 1;
        blit.srcSubresource.baseArrayLayer = 0;
        blit.srcSubresource.layerCount = 6;
        blit.srcOffsets[0] = {0, 0, 0};
        blit.srcOffsets[1] = {mip_width, mip_height, 1};
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.mipLevel = mip;
        blit.dstSubresource.baseArrayLayer = 0;
        blit.dstSubresource.layerCount = 6;
        blit.dstOffsets[0] = {0, 0, 0};
        blit.dstOffsets[1] = {next_width, next_height, 1};

        VkBlitImageInfo2 blit_info{};
        blit_info.sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2;
        blit_info.srcImage = cubemap_image_;
        blit_info.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        blit_info.dstImage = cubemap_image_;
        blit_info.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        blit_info.regionCount = 1;
        blit_info.pRegions = &blit;
        blit_info.filter = VK_FILTER_LINEAR;

        vkCmdBlitImage2(cmd, &blit_info);

        // Transition this mip to TRANSFER_SRC for next iteration
        {
            VkImageMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = cubemap_image_;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.baseMipLevel = mip;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.baseArrayLayer = 0;
            barrier.subresourceRange.layerCount = 6;

            VkDependencyInfo dep{};
            dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(cmd, &dep);
        }

        mip_width = next_width;
        mip_height = next_height;
    }

    // Transition all mips from TRANSFER_SRC to SHADER_READ_ONLY_OPTIMAL
    {
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = cubemap_image_;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = MIP_LEVELS;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 6;

        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);
    }
}

void SkyCubemapPass::create_cubemap() {
    // Create cubemap image
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    ci.extent = {CUBEMAP_SIZE, CUBEMAP_SIZE, 1};
    ci.mipLevels = MIP_LEVELS;
    ci.arrayLayers = 6;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vk_check(create_vma_image(allocator_, &ci, &cubemap_image_, &cubemap_memory_),
             "Failed to create sky cubemap image");

    // Cube view for sampling (all mips, all layers)
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = cubemap_image_;
    vi.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    vi.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.baseMipLevel = 0;
    vi.subresourceRange.levelCount = MIP_LEVELS;
    vi.subresourceRange.baseArrayLayer = 0;
    vi.subresourceRange.layerCount = 6;
    vk_check(vkCreateImageView(device_, &vi, nullptr, &cubemap_view_),
             "Failed to create sky cubemap cube view");

    // 2D array view for compute storage writes (mip 0 only)
    VkImageViewCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    si.image = cubemap_image_;
    si.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    si.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    si.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    si.subresourceRange.baseMipLevel = 0;
    si.subresourceRange.levelCount = 1;
    si.subresourceRange.baseArrayLayer = 0;
    si.subresourceRange.layerCount = 6;
    vk_check(vkCreateImageView(device_, &si, nullptr, &cubemap_storage_view_),
             "Failed to create sky cubemap storage view");

    // Sampler with trilinear filtering for mip sampling
    VkSamplerCreateInfo samp{};
    samp.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samp.magFilter = VK_FILTER_LINEAR;
    samp.minFilter = VK_FILTER_LINEAR;
    samp.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samp.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samp.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samp.maxLod = static_cast<float>(MIP_LEVELS);
    vk_check(vkCreateSampler(device_, &samp, nullptr, &cubemap_sampler_),
             "Failed to create sky cubemap sampler");
}

void SkyCubemapPass::create_brdf_lut() {
    // Create BRDF LUT image
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R16G16_SFLOAT;
    ci.extent = {BRDF_LUT_SIZE, BRDF_LUT_SIZE, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vk_check(create_vma_image(allocator_, &ci, &brdf_lut_image_, &brdf_lut_memory_),
             "Failed to create BRDF LUT image");

    // Image view
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = brdf_lut_image_;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R16G16_SFLOAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk_check(vkCreateImageView(device_, &vi, nullptr, &brdf_lut_view_),
             "Failed to create BRDF LUT view");

    // Sampler
    VkSamplerCreateInfo samp{};
    samp.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samp.magFilter = VK_FILTER_LINEAR;
    samp.minFilter = VK_FILTER_LINEAR;
    samp.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samp.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vk_check(vkCreateSampler(device_, &samp, nullptr, &brdf_lut_sampler_),
             "Failed to create BRDF LUT sampler");
}

void SkyCubemapPass::create_cubemap_pipeline() {
    // Descriptor set layout: binding 0 = storage image (2D array)
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_ci.bindingCount = 1;
    layout_ci.pBindings = &binding;
    vk_check(vkCreateDescriptorSetLayout(device_, &layout_ci, nullptr, &cubemap_set_layout_),
             "Failed to create cubemap descriptor set layout");

    // Descriptor pool
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    pool_size.descriptorCount = 1;

    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.maxSets = 1;
    pool_ci.poolSizeCount = 1;
    pool_ci.pPoolSizes = &pool_size;
    vk_check(vkCreateDescriptorPool(device_, &pool_ci, nullptr, &cubemap_pool_),
             "Failed to create cubemap descriptor pool");

    // Allocate descriptor set
    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = cubemap_pool_;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &cubemap_set_layout_;
    vk_check(vkAllocateDescriptorSets(device_, &alloc_info, &cubemap_set_),
             "Failed to allocate cubemap descriptor set");

    // Write storage image descriptor
    VkDescriptorImageInfo img_info{};
    img_info.imageView = cubemap_storage_view_;
    img_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = cubemap_set_;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.descriptorCount = 1;
    write.pImageInfo = &img_info;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    // Push constants: top_color (vec4) + bottom_color (vec4) + face_size (uint)
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(glm::vec4) * 2 + sizeof(uint32_t);

    // Pipeline layout
    VkPipelineLayoutCreateInfo pl_ci{};
    pl_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl_ci.setLayoutCount = 1;
    pl_ci.pSetLayouts = &cubemap_set_layout_;
    pl_ci.pushConstantRangeCount = 1;
    pl_ci.pPushConstantRanges = &push_range;
    vk_check(vkCreatePipelineLayout(device_, &pl_ci, nullptr, &cubemap_pipeline_layout_),
             "Failed to create cubemap pipeline layout");

    // Compute pipeline
    auto code = read_spirv(base_path::resolve("shaders/sky_cubemap.comp.spv"));
    auto module = create_shader_module(device_, code);

    VkComputePipelineCreateInfo comp_ci{};
    comp_ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    comp_ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    comp_ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    comp_ci.stage.module = module;
    comp_ci.stage.pName = "main";
    comp_ci.layout = cubemap_pipeline_layout_;
    vk_check(vkCreateComputePipelines(device_, vk_pipeline_cache_, 1, &comp_ci,
                                       nullptr, &cubemap_pipeline_),
             "Failed to create cubemap compute pipeline");

    vkDestroyShaderModule(device_, module, nullptr);
}

void SkyCubemapPass::create_brdf_pipeline() {
    // Descriptor set layout: binding 0 = storage image (2D)
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_ci.bindingCount = 1;
    layout_ci.pBindings = &binding;
    vk_check(vkCreateDescriptorSetLayout(device_, &layout_ci, nullptr, &brdf_set_layout_),
             "Failed to create BRDF descriptor set layout");

    // Descriptor pool
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    pool_size.descriptorCount = 1;

    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.maxSets = 1;
    pool_ci.poolSizeCount = 1;
    pool_ci.pPoolSizes = &pool_size;
    vk_check(vkCreateDescriptorPool(device_, &pool_ci, nullptr, &brdf_pool_),
             "Failed to create BRDF descriptor pool");

    // Allocate descriptor set
    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = brdf_pool_;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &brdf_set_layout_;
    vk_check(vkAllocateDescriptorSets(device_, &alloc_info, &brdf_set_),
             "Failed to allocate BRDF descriptor set");

    // Write storage image descriptor
    VkDescriptorImageInfo img_info{};
    img_info.imageView = brdf_lut_view_;
    img_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = brdf_set_;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.descriptorCount = 1;
    write.pImageInfo = &img_info;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    // Pipeline layout (no push constants for BRDF)
    VkPipelineLayoutCreateInfo pl_ci{};
    pl_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl_ci.setLayoutCount = 1;
    pl_ci.pSetLayouts = &brdf_set_layout_;
    vk_check(vkCreatePipelineLayout(device_, &pl_ci, nullptr, &brdf_pipeline_layout_),
             "Failed to create BRDF pipeline layout");

    // Compute pipeline
    auto code = read_spirv(base_path::resolve("shaders/brdf_lut.comp.spv"));
    auto module = create_shader_module(device_, code);

    VkComputePipelineCreateInfo comp_ci{};
    comp_ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    comp_ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    comp_ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    comp_ci.stage.module = module;
    comp_ci.stage.pName = "main";
    comp_ci.layout = brdf_pipeline_layout_;
    vk_check(vkCreateComputePipelines(device_, vk_pipeline_cache_, 1, &comp_ci,
                                       nullptr, &brdf_pipeline_),
             "Failed to create BRDF compute pipeline");

    vkDestroyShaderModule(device_, module, nullptr);
}

void SkyCubemapPass::generate_brdf_lut() {
    // One-shot command buffer to generate the BRDF LUT at startup
    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo cmd_ai{};
    cmd_ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_ai.commandPool = command_pool_;
    cmd_ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_ai.commandBufferCount = 1;
    vk_check(vkAllocateCommandBuffers(device_, &cmd_ai, &cmd),
             "Failed to allocate BRDF LUT command buffer");

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(cmd, &begin),
             "Failed to begin BRDF LUT command buffer");

    // Transition BRDF LUT to GENERAL for compute write
    {
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        barrier.srcAccessMask = 0;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = brdf_lut_image_;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    // Dispatch BRDF LUT compute
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, brdf_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            brdf_pipeline_layout_, 0, 1, &brdf_set_, 0, nullptr);

    uint32_t groups = (BRDF_LUT_SIZE + 7) / 8;
    vkCmdDispatch(cmd, groups, groups, 1);

    // Transition BRDF LUT to SHADER_READ_ONLY
    {
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = brdf_lut_image_;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    vk_check(vkEndCommandBuffer(cmd),
             "Failed to end BRDF LUT command buffer");

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vk_check(vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE),
             "Failed to submit BRDF LUT generation");
    vkQueueWaitIdle(queue_);
    vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);

    FJELL_GFX_DEBUG("BRDF LUT generated ({}x{})", BRDF_LUT_SIZE, BRDF_LUT_SIZE);
}

} // namespace fjell
