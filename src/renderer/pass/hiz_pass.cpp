#include "renderer/pass/hiz_pass.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/gpu/vma_image.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/resources/shader_utils.hpp"
#include "renderer/gpu/vk_check.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace fjell {

// ── Images (per-viewport state) ─────────────────────────────────────────────

void HizPass::Images::create(VkDevice device, VmaAllocator allocator,
                               VkDescriptorSetLayout set_layout, VkSampler sampler,
                               uint32_t width, uint32_t height) {
    // Hi-Z mip 0 = half-res
    hiz_width = std::max(width / 2, 1u);
    hiz_height = std::max(height / 2, 1u);
    hiz_mip_count = static_cast<uint32_t>(
        std::floor(std::log2(std::max(hiz_width, hiz_height)))) + 1;
    hiz_mip_count = std::min(hiz_mip_count, MAX_HIZ_MIPS);

    VkImageCreateInfo image_ci{};
    image_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_ci.imageType = VK_IMAGE_TYPE_2D;
    image_ci.format = VK_FORMAT_R32_SFLOAT;
    image_ci.extent = {hiz_width, hiz_height, 1};
    image_ci.mipLevels = hiz_mip_count;
    image_ci.arrayLayers = 1;
    image_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    image_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    vk_check(create_vma_image(allocator, &image_ci, &hiz_image, &hiz_memory),
             "Failed to create Hi-Z image");

    // Sampled view: all mips
    VkImageViewCreateInfo view_ci{};
    view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_ci.image = hiz_image;
    view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_ci.format = VK_FORMAT_R32_SFLOAT;
    view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, hiz_mip_count, 0, 1};
    vk_check(vkCreateImageView(device, &view_ci, nullptr, &hiz_sampled_view),
             "Failed to create Hi-Z sampled view");

    // Per-mip views for storage writes
    for (uint32_t i = 0; i < hiz_mip_count; ++i) {
        view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, i, 1, 0, 1};
        vk_check(vkCreateImageView(device, &view_ci, nullptr, &hiz_mip_views[i]),
                 "Failed to create Hi-Z mip view");
    }

    // Own descriptor pool + per-mip sets
    std::array<VkDescriptorPoolSize, 2> pool_sizes{};
    pool_sizes[0] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_HIZ_MIPS};
    pool_sizes[1] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, MAX_HIZ_MIPS};

    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.maxSets = MAX_HIZ_MIPS;
    pool_ci.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
    pool_ci.pPoolSizes = pool_sizes.data();
    vk_check(vkCreateDescriptorPool(device, &pool_ci, nullptr, &pool),
             "Failed to create Hi-Z images descriptor pool");

    for (uint32_t i = 0; i < MAX_HIZ_MIPS; ++i) {
        VkDescriptorSetAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = pool;
        alloc_info.descriptorSetCount = 1;
        alloc_info.pSetLayouts = &set_layout;
        vk_check(vkAllocateDescriptorSets(device, &alloc_info, &mip_sets[i]),
                 "Failed to allocate Hi-Z mip descriptor set");
    }

    // Pre-write the storage image descriptors (binding 1) for each mip
    for (uint32_t i = 0; i < hiz_mip_count; ++i) {
        VkDescriptorImageInfo dst_info{};
        dst_info.imageView = hiz_mip_views[i];
        dst_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = mip_sets[i];
        write.dstBinding = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        write.descriptorCount = 1;
        write.pImageInfo = &dst_info;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }

    // Pre-write source descriptors (binding 0) for mips 1+
    // (mip 0 source is the depth buffer, updated per-frame in record_into)
    for (uint32_t i = 1; i < hiz_mip_count; ++i) {
        VkDescriptorImageInfo src_info{};
        src_info.sampler = sampler;
        src_info.imageView = hiz_mip_views[i - 1];
        src_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = mip_sets[i];
        write.dstBinding = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &src_info;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }

}

void HizPass::Images::destroy(VkDevice device, VmaAllocator allocator) {
    if (pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, pool, nullptr);
        pool = VK_NULL_HANDLE;
        mip_sets = {};
    }
    for (uint32_t i = 0; i < hiz_mip_count; ++i) {
        if (hiz_mip_views[i] != VK_NULL_HANDLE) {
            vkDestroyImageView(device, hiz_mip_views[i], nullptr);
            hiz_mip_views[i] = VK_NULL_HANDLE;
        }
    }
    if (hiz_sampled_view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, hiz_sampled_view, nullptr);
        hiz_sampled_view = VK_NULL_HANDLE;
    }
    destroy_vma_image(allocator, hiz_image, hiz_memory);
    hiz_image = VK_NULL_HANDLE;
    hiz_memory = VK_NULL_HANDLE;
    hiz_mip_count = 0;
    hiz_width = 0;
    hiz_height = 0;
    last_depth_view = VK_NULL_HANDLE;
}

void HizPass::Images::resize(VkDevice device, VmaAllocator allocator,
                               VkDescriptorSetLayout set_layout, VkSampler sampler,
                               uint32_t width, uint32_t height) {
    uint32_t new_w = std::max(width / 2, 1u);
    uint32_t new_h = std::max(height / 2, 1u);
    if (new_w == hiz_width && new_h == hiz_height) return;
    vkDeviceWaitIdle(device);

    destroy(device, allocator);
    create(device, allocator, set_layout, sampler, width, height);
}

// ── HizPass (singleton) ─────────────────────────────────────────────────────

Result<> HizPass::create(Device& device, const PassResources& resources) {
    device_ = device.handle();
    allocator_ = resources.allocator;

    // Sampler: nearest filter, clamp to edge, all mips
    VkSamplerCreateInfo sampler_ci{};
    sampler_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_ci.magFilter = VK_FILTER_NEAREST;
    sampler_ci.minFilter = VK_FILTER_NEAREST;
    sampler_ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.maxLod = static_cast<float>(MAX_HIZ_MIPS);
    vk_check(vkCreateSampler(device_, &sampler_ci, nullptr, &sampler_),
             "Failed to create Hi-Z sampler");

    // Descriptor layout: binding 0 = sampler (source), binding 1 = storage image (dest mip)
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_ci.bindingCount = static_cast<uint32_t>(bindings.size());
    layout_ci.pBindings = bindings.data();
    vk_check(vkCreateDescriptorSetLayout(device_, &layout_ci, nullptr, &set_layout_),
             "Failed to create Hi-Z descriptor set layout");

    // Pipeline layout: single set + push constants
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset = 0;
    push_range.size = 4 * sizeof(uint32_t); // src_w, src_h, dst_w, dst_h

    VkPipelineLayoutCreateInfo pipe_layout_ci{};
    pipe_layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipe_layout_ci.setLayoutCount = 1;
    pipe_layout_ci.pSetLayouts = &set_layout_;
    pipe_layout_ci.pushConstantRangeCount = 1;
    pipe_layout_ci.pPushConstantRanges = &push_range;

    vk_check(vkCreatePipelineLayout(device_, &pipe_layout_ci, nullptr, &layout_),
             "Failed to create Hi-Z pipeline layout");

    // Compute pipeline
    auto code = read_spirv(base_path::resolve("shaders/hiz_build.comp.spv"));
    auto module = create_shader_module(device_, code);

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module;
    stage.pName = "main";

    VkComputePipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage = stage;
    ci.layout = layout_;

    vk_check(vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline_),
             "Failed to create Hi-Z compute pipeline");
    vkDestroyShaderModule(device_, module, nullptr);

    FJELL_GFX_DEBUG("HizPass created");
    return {};
}

void HizPass::destroy() {
    default_images_.destroy(device_, allocator_);
    if (pipeline_ != VK_NULL_HANDLE) { vkDestroyPipeline(device_, pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE; }
    if (layout_ != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device_, layout_, nullptr); layout_ = VK_NULL_HANDLE; }
    if (set_layout_ != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr); set_layout_ = VK_NULL_HANDLE; }
    if (sampler_ != VK_NULL_HANDLE) { vkDestroySampler(device_, sampler_, nullptr); sampler_ = VK_NULL_HANDLE; }
}

void HizPass::resize(uint32_t width, uint32_t height) {
    default_images_.resize(device_, allocator_, set_layout_, sampler_, width, height);
}

void HizPass::record(const FrameContext& ctx) {
    record_into(ctx, default_images_);
}

void HizPass::record_into(const FrameContext& ctx, Images& imgs) {
    auto extent = ctx.target->extent();
    uint32_t depth_width = extent.width;
    uint32_t depth_height = extent.height;
    if (depth_width == 0 || depth_height == 0) return;

    VkImageView depth_view = ctx.target->depth_view();

    // Lazy create / resize
    uint32_t needed_w = std::max(depth_width / 2, 1u);
    uint32_t needed_h = std::max(depth_height / 2, 1u);
    if (needed_w != imgs.hiz_width || needed_h != imgs.hiz_height) {
        imgs.destroy(device_, allocator_);
        imgs.create(device_, allocator_, set_layout_, sampler_, depth_width, depth_height);
    }
    if (imgs.hiz_mip_count == 0) return;

    auto cmd = ctx.cmd;

    // Transition all Hi-Z mips to GENERAL for storage writes
    VkImageMemoryBarrier2 init_barrier{};
    init_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    init_barrier.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    init_barrier.srcAccessMask = 0;
    init_barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    init_barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    init_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    init_barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    init_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    init_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    init_barrier.image = imgs.hiz_image;
    init_barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, imgs.hiz_mip_count, 0, 1};

    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &init_barrier;
    vkCmdPipelineBarrier2(cmd, &dep);

    // Update mip 0 source descriptor: depth buffer (only when view changes)
    if (depth_view != imgs.last_depth_view) {
        imgs.last_depth_view = depth_view;

        VkDescriptorImageInfo depth_info{};
        depth_info.sampler = sampler_;
        depth_info.imageView = depth_view;
        depth_info.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet depth_write{};
        depth_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        depth_write.dstSet = imgs.mip_sets[0];
        depth_write.dstBinding = 0;
        depth_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        depth_write.descriptorCount = 1;
        depth_write.pImageInfo = &depth_info;
        vkUpdateDescriptorSets(device_, 1, &depth_write, 0, nullptr);
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);

    uint32_t src_w = depth_width;
    uint32_t src_h = depth_height;

    for (uint32_t mip = 0; mip < imgs.hiz_mip_count; ++mip) {
        uint32_t dst_w = std::max(imgs.hiz_width >> mip, 1u);
        uint32_t dst_h = std::max(imgs.hiz_height >> mip, 1u);

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_,
                                0, 1, &imgs.mip_sets[mip], 0, nullptr);

        uint32_t push[4] = {src_w, src_h, dst_w, dst_h};
        vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(push), push);

        uint32_t gx = (dst_w + 7) / 8;
        uint32_t gy = (dst_h + 7) / 8;
        vkCmdDispatch(cmd, gx, gy, 1);

        // Barrier: this mip written → next mip reads it as sampled texture
        VkImageMemoryBarrier2 mip_barrier{};
        mip_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        mip_barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mip_barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        mip_barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mip_barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        mip_barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        mip_barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        mip_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        mip_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        mip_barrier.image = imgs.hiz_image;
        mip_barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 1};

        VkDependencyInfo mip_dep{};
        mip_dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        mip_dep.imageMemoryBarrierCount = 1;
        mip_dep.pImageMemoryBarriers = &mip_barrier;
        vkCmdPipelineBarrier2(cmd, &mip_dep);

        src_w = dst_w;
        src_h = dst_h;
    }
}

} // namespace fjell
