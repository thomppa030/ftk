#include "renderer/pass/gtao_pass.hpp"
#include "renderer/gpu/vma_image.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/post_process_resources.hpp"
#include "renderer/resource_registry.hpp"
#include "renderer/resources/shader_utils.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <array>
#include <stdexcept>

namespace fjell {

static constexpr uint32_t TILE_SIZE = 8;
static constexpr VkFormat AO_FORMAT = VK_FORMAT_R8_UNORM;

struct GtaoPushConstants {
    float radius;
    float intensity;
    float thickness;
    float max_pixel_radius;
    uint32_t viewport_width;
    uint32_t viewport_height;
    uint32_t full_viewport_width;
    uint32_t full_viewport_height;
};

struct BlurPushConstants {
    uint32_t viewport_width;
    uint32_t viewport_height;
    uint32_t direction; // 0 = horizontal, 1 = vertical
    float depth_threshold;
};

// ── Images (per-viewport state) ─────────────────────────────────────────────

static void create_ao_image(VkDevice device, VmaAllocator allocator,
                             uint32_t width, uint32_t height,
                             VkImage& image, VmaAllocation& allocation, VkImageView& view) {
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = AO_FORMAT;
    ci.extent = {width, height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (create_vma_image(allocator, &ci, &image, &allocation) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create GTAO image");
    }

    VkImageViewCreateInfo view_ci{};
    view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_ci.image = image;
    view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_ci.format = AO_FORMAT;
    view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    if (vkCreateImageView(device, &view_ci, nullptr, &view) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create GTAO image view");
    }
}

static void destroy_ao_image(VkDevice device, VmaAllocator allocator,
                              VkImage& image, VmaAllocation& allocation, VkImageView& view) {
    if (view != VK_NULL_HANDLE) { vkDestroyImageView(device, view, nullptr); view = VK_NULL_HANDLE; }
    destroy_vma_image(allocator, image, allocation);
    image = VK_NULL_HANDLE; allocation = VK_NULL_HANDLE;
}

void GtaoPass::Images::create(VkDevice device, VmaAllocator allocator,
                                VkDescriptorSetLayout set_layout, VkSampler sampler,
                                uint32_t half_w, uint32_t half_h) {
    extent = {half_w, half_h};

    create_ao_image(device, allocator, half_w, half_h, ao_image, ao_memory, ao_view);
    create_ao_image(device, allocator, half_w, half_h, blur_image, blur_memory, blur_view);

    images_need_init = true;

    // Own descriptor pool (3 sets: gtao, blur_h, blur_v)
    std::array<VkDescriptorPoolSize, 2> pool_sizes{};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    pool_sizes[0].descriptorCount = 3;
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_sizes[1].descriptorCount = 3;

    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.maxSets = 3;
    pool_ci.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
    pool_ci.pPoolSizes = pool_sizes.data();

    if (vkCreateDescriptorPool(device, &pool_ci, nullptr, &pool) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create GTAO images descriptor pool");
    }

    std::array<VkDescriptorSetLayout, 3> layouts = {set_layout, set_layout, set_layout};
    VkDescriptorSetAllocateInfo alloc_ci{};
    alloc_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_ci.descriptorPool = pool;
    alloc_ci.descriptorSetCount = 3;
    alloc_ci.pSetLayouts = layouts.data();

    std::array<VkDescriptorSet, 3> sets{};
    if (vkAllocateDescriptorSets(device, &alloc_ci, sets.data()) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate GTAO images descriptor sets");
    }
    gtao_set = sets[0];
    blur_h_set = sets[1];
    blur_v_set = sets[2];

    // Write descriptors
    // gtao_set: binding 0 = ao_image (storage write)
    {
        VkDescriptorImageInfo storage_info{};
        storage_info.imageView = ao_view;
        storage_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = gtao_set;
        write.dstBinding = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        write.descriptorCount = 1;
        write.pImageInfo = &storage_info;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }

    // blur_h_set: binding 0 = blur_image (storage write), binding 1 = ao_image (sampled read)
    {
        VkDescriptorImageInfo storage_info{};
        storage_info.imageView = blur_view;
        storage_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo sampled_info{};
        sampled_info.sampler = sampler;
        sampled_info.imageView = ao_view;
        sampled_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        std::array<VkWriteDescriptorSet, 2> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = blur_h_set;
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].descriptorCount = 1;
        writes[0].pImageInfo = &storage_info;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = blur_h_set;
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo = &sampled_info;

        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }

    // blur_v_set: binding 0 = ao_image (storage write), binding 1 = blur_image (sampled read)
    {
        VkDescriptorImageInfo storage_info{};
        storage_info.imageView = ao_view;
        storage_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo sampled_info{};
        sampled_info.sampler = sampler;
        sampled_info.imageView = blur_view;
        sampled_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        std::array<VkWriteDescriptorSet, 2> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = blur_v_set;
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].descriptorCount = 1;
        writes[0].pImageInfo = &storage_info;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = blur_v_set;
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo = &sampled_info;

        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
}

void GtaoPass::Images::destroy(VkDevice device, VmaAllocator allocator) {
    invalidate_imgui_texture();
    if (pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, pool, nullptr);
        pool = VK_NULL_HANDLE;
        gtao_set = VK_NULL_HANDLE;
        blur_h_set = VK_NULL_HANDLE;
        blur_v_set = VK_NULL_HANDLE;
    }
    destroy_ao_image(device, allocator, ao_image, ao_memory, ao_view);
    destroy_ao_image(device, allocator, blur_image, blur_memory, blur_view);
    extent = {};
}

void GtaoPass::Images::resize(VkDevice device, VmaAllocator allocator,
                                VkDescriptorSetLayout set_layout, VkSampler sampler,
                                uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;

    uint32_t half_w = std::max(1u, width / 2);
    uint32_t half_h = std::max(1u, height / 2);
    if (extent.width == half_w && extent.height == half_h) return;
    vkDeviceWaitIdle(device);

    destroy(device, allocator);
    create(device, allocator, set_layout, sampler, half_w, half_h);
}

VkDescriptorSet GtaoPass::Images::imgui_texture(VkSampler sampler) {
    if (imgui_descriptor == VK_NULL_HANDLE && ao_view != VK_NULL_HANDLE) {
        imgui_descriptor = ImGui_ImplVulkan_AddTexture(
            sampler, ao_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return imgui_descriptor;
}

void GtaoPass::Images::invalidate_imgui_texture() {
    if (imgui_descriptor != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(imgui_descriptor);
        imgui_descriptor = VK_NULL_HANDLE;
    }
}

// ── GtaoPass (singleton) ────────────────────────────────────────────────────

Result<> GtaoPass::create(Device& device, const PassResources& resources) {
    device_ = device.handle();
    vk_pipeline_cache_ = resources.pipeline_cache;
    allocator_ = resources.allocator;
    auto global_set_layout = resources.global_set_layout;

    // Sampler for AO texture (linear, clamp)
    VkSamplerCreateInfo sampler_ci{};
    sampler_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_ci.magFilter = VK_FILTER_LINEAR;
    sampler_ci.minFilter = VK_FILTER_LINEAR;
    sampler_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device_, &sampler_ci, nullptr, &ao_sampler_) != VK_SUCCESS) {
        return make_error("Failed to create GTAO sampler");
    }

    // Pass descriptor set layout: binding 0 = storage image (write), binding 1 = combined image sampler (read)
    std::array<VkDescriptorSetLayoutBinding, 2> pass_bindings{};
    pass_bindings[0].binding = 0;
    pass_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    pass_bindings[0].descriptorCount = 1;
    pass_bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    pass_bindings[1].binding = 1;
    pass_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pass_bindings[1].descriptorCount = 1;
    pass_bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo pass_layout_ci{};
    pass_layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    pass_layout_ci.bindingCount = static_cast<uint32_t>(pass_bindings.size());
    pass_layout_ci.pBindings = pass_bindings.data();

    if (vkCreateDescriptorSetLayout(device_, &pass_layout_ci, nullptr, &pass_set_layout_) != VK_SUCCESS) {
        return make_error("Failed to create GTAO pass descriptor set layout");
    }

    // GTAO pipeline layout: set 0 = global, set 1 = pass
    {
        std::array<VkDescriptorSetLayout, 2> set_layouts = {global_set_layout, pass_set_layout_};

        VkPushConstantRange push_range{};
        push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push_range.offset = 0;
        push_range.size = sizeof(GtaoPushConstants);

        VkPipelineLayoutCreateInfo layout_ci{};
        layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_ci.setLayoutCount = static_cast<uint32_t>(set_layouts.size());
        layout_ci.pSetLayouts = set_layouts.data();
        layout_ci.pushConstantRangeCount = 1;
        layout_ci.pPushConstantRanges = &push_range;

        if (vkCreatePipelineLayout(device_, &layout_ci, nullptr, &gtao_layout_) != VK_SUCCESS) {
            return make_error("Failed to create GTAO pipeline layout");
        }

        auto comp_code = read_spirv(base_path::resolve("shaders/gtao.comp.spv"));
        auto comp_module = create_shader_module(device_, comp_code);

        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = comp_module;
        stage.pName = "main";

        VkComputePipelineCreateInfo pipeline_ci{};
        pipeline_ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipeline_ci.stage = stage;
        pipeline_ci.layout = gtao_layout_;

        if (vkCreateComputePipelines(device_, vk_pipeline_cache_, 1, &pipeline_ci,
                                      nullptr, &gtao_pipeline_) != VK_SUCCESS) {
            vkDestroyShaderModule(device_, comp_module, nullptr);
            return make_error("Failed to create GTAO compute pipeline");
        }
        vkDestroyShaderModule(device_, comp_module, nullptr);
    }

    // Blur pipeline layout: set 0 = global (for depth), set 1 = pass (read/write)
    {
        std::array<VkDescriptorSetLayout, 2> set_layouts = {global_set_layout, pass_set_layout_};

        VkPushConstantRange push_range{};
        push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push_range.offset = 0;
        push_range.size = sizeof(BlurPushConstants);

        VkPipelineLayoutCreateInfo layout_ci{};
        layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_ci.setLayoutCount = static_cast<uint32_t>(set_layouts.size());
        layout_ci.pSetLayouts = set_layouts.data();
        layout_ci.pushConstantRangeCount = 1;
        layout_ci.pPushConstantRanges = &push_range;

        if (vkCreatePipelineLayout(device_, &layout_ci, nullptr, &blur_layout_) != VK_SUCCESS) {
            return make_error("Failed to create GTAO blur pipeline layout");
        }

        auto comp_code = read_spirv(base_path::resolve("shaders/gtao_blur.comp.spv"));
        auto comp_module = create_shader_module(device_, comp_code);

        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = comp_module;
        stage.pName = "main";

        VkComputePipelineCreateInfo pipeline_ci{};
        pipeline_ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipeline_ci.stage = stage;
        pipeline_ci.layout = blur_layout_;

        if (vkCreateComputePipelines(device_, vk_pipeline_cache_, 1, &pipeline_ci,
                                      nullptr, &blur_pipeline_) != VK_SUCCESS) {
            vkDestroyShaderModule(device_, comp_module, nullptr);
            return make_error("Failed to create GTAO blur compute pipeline");
        }
        vkDestroyShaderModule(device_, comp_module, nullptr);
    }

    // Create 1x1 placeholder images so binding 5 is valid from frame 1
    default_images_.create(device_, allocator_, pass_set_layout_, ao_sampler_, 1, 1);

    FJELL_GFX_DEBUG("GTAO pass created");
    return {};
}

void GtaoPass::destroy() {
    default_images_.destroy(device_, allocator_);

    if (ao_sampler_ != VK_NULL_HANDLE) { vkDestroySampler(device_, ao_sampler_, nullptr); ao_sampler_ = VK_NULL_HANDLE; }
    if (gtao_pipeline_ != VK_NULL_HANDLE) { vkDestroyPipeline(device_, gtao_pipeline_, nullptr); gtao_pipeline_ = VK_NULL_HANDLE; }
    if (gtao_layout_ != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device_, gtao_layout_, nullptr); gtao_layout_ = VK_NULL_HANDLE; }
    if (blur_pipeline_ != VK_NULL_HANDLE) { vkDestroyPipeline(device_, blur_pipeline_, nullptr); blur_pipeline_ = VK_NULL_HANDLE; }
    if (blur_layout_ != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device_, blur_layout_, nullptr); blur_layout_ = VK_NULL_HANDLE; }
    if (pass_set_layout_ != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device_, pass_set_layout_, nullptr); pass_set_layout_ = VK_NULL_HANDLE; }
}

void GtaoPass::resize(uint32_t width, uint32_t height) {
    default_images_.resize(device_, allocator_, pass_set_layout_, ao_sampler_, width, height);
}

VkDescriptorSet GtaoPass::imgui_texture() {
    return default_images_.imgui_texture(ao_sampler_);
}

void GtaoPass::invalidate_imgui_texture() {
    default_images_.invalidate_imgui_texture();
}

void GtaoPass::record(const FrameContext& ctx) {
    record_into(ctx, default_images_);
}

void GtaoPass::record_into(const FrameContext& ctx, Images& imgs) {
    if (imgs.ao_view == VK_NULL_HANDLE) return;

    auto cmd = ctx.cmd;
    auto global_set = ctx.global_descriptor_set;
    auto target_extent = ctx.target->extent();
    uint32_t width = target_extent.width;
    uint32_t height = target_extent.height;

    // Transition images from UNDEFINED on first use (after create or resize)
    if (imgs.images_need_init) {
        std::array<VkImageMemoryBarrier2, 2> init_barriers{};

        init_barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        init_barriers[0].srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        init_barriers[0].srcAccessMask = 0;
        init_barriers[0].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        init_barriers[0].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        init_barriers[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        init_barriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        init_barriers[0].image = imgs.ao_image;
        init_barriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        init_barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        init_barriers[1].srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        init_barriers[1].srcAccessMask = 0;
        init_barriers[1].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        init_barriers[1].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        init_barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        init_barriers[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        init_barriers[1].image = imgs.blur_image;
        init_barriers[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        VkDependencyInfo init_dep{};
        init_dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        init_dep.imageMemoryBarrierCount = static_cast<uint32_t>(init_barriers.size());
        init_dep.pImageMemoryBarriers = init_barriers.data();
        vkCmdPipelineBarrier2(cmd, &init_dep);

        imgs.images_need_init = false;
    }

    if (!enabled_) {
        // Clear AO to white (1.0 = no occlusion)
        VkImageMemoryBarrier2 to_transfer{};
        to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        to_transfer.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        to_transfer.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        to_transfer.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        to_transfer.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        to_transfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_transfer.image = imgs.ao_image;
        to_transfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &to_transfer;
        vkCmdPipelineBarrier2(cmd, &dep);

        VkClearColorValue clear_value{};
        clear_value.float32[0] = 1.0f;
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, imgs.ao_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &clear_value, 1, &range);

        VkImageMemoryBarrier2 to_read{};
        to_read.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        to_read.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        to_read.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        to_read.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        to_read.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        to_read.image = imgs.ao_image;
        to_read.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        dep.pImageMemoryBarriers = &to_read;
        vkCmdPipelineBarrier2(cmd, &dep);
        return;
    }

    // Compute at half resolution
    uint32_t half_w = imgs.extent.width;
    uint32_t half_h = imgs.extent.height;
    uint32_t groups_x = (half_w + TILE_SIZE - 1) / TILE_SIZE;
    uint32_t groups_y = (half_h + TILE_SIZE - 1) / TILE_SIZE;

    // Transition both images to GENERAL for compute write
    {
        std::array<VkImageMemoryBarrier2, 2> to_general{};

        to_general[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        to_general[0].srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        to_general[0].srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        to_general[0].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        to_general[0].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        to_general[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_general[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        to_general[0].image = imgs.ao_image;
        to_general[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        to_general[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        to_general[1].srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        to_general[1].srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        to_general[1].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        to_general[1].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        to_general[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_general[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        to_general[1].image = imgs.blur_image;
        to_general[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = static_cast<uint32_t>(to_general.size());
        dep.pImageMemoryBarriers = to_general.data();
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    // GTAO compute
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gtao_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gtao_layout_,
                            0, 1, &global_set, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gtao_layout_,
                            1, 1, &imgs.gtao_set, 0, nullptr);

    GtaoPushConstants push{};
    push.radius = radius_;
    push.intensity = intensity_;
    push.thickness = thickness_;
    push.max_pixel_radius = 128.0f;
    push.viewport_width = half_w;
    push.viewport_height = half_h;
    push.full_viewport_width = width;
    push.full_viewport_height = height;

    vkCmdPushConstants(cmd, gtao_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);

    vkCmdDispatch(cmd, groups_x, groups_y, 1);

    // Barrier: GTAO write → blur H read
    VkImageMemoryBarrier2 ao_to_read{};
    ao_to_read.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    ao_to_read.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    ao_to_read.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    ao_to_read.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    ao_to_read.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    ao_to_read.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    ao_to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ao_to_read.image = imgs.ao_image;
    ao_to_read.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &ao_to_read;
    vkCmdPipelineBarrier2(cmd, &dep);

    // Blur horizontal
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, blur_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, blur_layout_,
                            0, 1, &global_set, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, blur_layout_,
                            1, 1, &imgs.blur_h_set, 0, nullptr);

    BlurPushConstants blur_push{};
    blur_push.viewport_width = half_w;
    blur_push.viewport_height = half_h;
    blur_push.direction = 0;
    blur_push.depth_threshold = 0.1f;

    vkCmdPushConstants(cmd, blur_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(blur_push), &blur_push);

    vkCmdDispatch(cmd, groups_x, groups_y, 1);

    // Barrier: blur H write → blur V read
    std::array<VkImageMemoryBarrier2, 2> blur_barriers{};

    blur_barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    blur_barriers[0].srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    blur_barriers[0].srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    blur_barriers[0].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    blur_barriers[0].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    blur_barriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    blur_barriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    blur_barriers[0].image = imgs.blur_image;
    blur_barriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    blur_barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    blur_barriers[1].srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    blur_barriers[1].srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    blur_barriers[1].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    blur_barriers[1].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    blur_barriers[1].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    blur_barriers[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    blur_barriers[1].image = imgs.ao_image;
    blur_barriers[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkDependencyInfo blur_dep{};
    blur_dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    blur_dep.imageMemoryBarrierCount = static_cast<uint32_t>(blur_barriers.size());
    blur_dep.pImageMemoryBarriers = blur_barriers.data();
    vkCmdPipelineBarrier2(cmd, &blur_dep);

    // Blur vertical
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, blur_layout_,
                            1, 1, &imgs.blur_v_set, 0, nullptr);

    blur_push.direction = 1;
    vkCmdPushConstants(cmd, blur_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(blur_push), &blur_push);

    vkCmdDispatch(cmd, groups_x, groups_y, 1);

    // Final barrier: ao_image GENERAL → SHADER_READ_ONLY for mesh.frag
    VkImageMemoryBarrier2 ao_final{};
    ao_final.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    ao_final.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    ao_final.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    ao_final.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    ao_final.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    ao_final.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    ao_final.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ao_final.image = imgs.ao_image;
    ao_final.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkDependencyInfo final_dep{};
    final_dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    final_dep.imageMemoryBarrierCount = 1;
    final_dep.pImageMemoryBarriers = &ao_final;
    vkCmdPipelineBarrier2(cmd, &final_dep);
}

PassSetup GtaoPass::setup() const {
    return {
        .name = "gtao",
        .phase = RenderPhase::GpuDriven,
        .sub_order = 100,
        .reads = {{"depth", ImageUsage::compute_read}},
        .writes = {{"ao", ImageUsage::compute_write}},
    };
}

void GtaoPass::declare(PassBuilder& /*builder*/, const DeclareContext& /*ctx*/) {
    // Per-frame topology (depth import) is produced inside build() below
    // until Phase 2 routes declare() through the pipeline.
}

void GtaoPass::build(FrameGraph& graph, ResourceRegistry& /*registry*/,
                      const FrameContext& ctx) {
    auto* target = ctx.target;
    if (target == nullptr) { return; }

    PassBuilder builder;
    auto depth = builder.import("depth", target->depth_image(), target->depth_view(),
                                 VK_IMAGE_ASPECT_DEPTH_BIT);
    builder.read(depth, ResourceAccess::sampled_compute);

    graph.submit_declared_pass("gtao", builder,
        [this, ctx](VkCommandBuffer cb) {
            FrameContext local = ctx;
            local.cmd = cb;
            if (local.pp_resources && local.pp_resources->gtao()) {
                record_into(local, *local.pp_resources->gtao());
            }
        });
}

} // namespace fjell
