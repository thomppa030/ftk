#include "renderer/pass/gtao_pass.hpp"
#include "renderer/buffer.hpp"
#include "renderer/shader_utils.hpp"
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

void GtaoPass::create(VkDevice device, VkPhysicalDevice physical_device,
                         VkDescriptorSetLayout global_set_layout) {
    device_ = device;
    physical_device_ = physical_device;

    // Sampler for AO texture (linear, clamp)
    VkSamplerCreateInfo sampler_ci{};
    sampler_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_ci.magFilter = VK_FILTER_LINEAR;
    sampler_ci.minFilter = VK_FILTER_LINEAR;
    sampler_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device_, &sampler_ci, nullptr, &ao_sampler_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create GTAO sampler");
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
        throw std::runtime_error("Failed to create GTAO pass descriptor set layout");
    }

    // Descriptor pool (3 sets: gtvbao, blur_h, blur_v)
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

    if (vkCreateDescriptorPool(device_, &pool_ci, nullptr, &pass_pool_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create GTAO descriptor pool");
    }

    // Allocate 3 descriptor sets
    std::array<VkDescriptorSetLayout, 3> layouts = {pass_set_layout_, pass_set_layout_, pass_set_layout_};
    VkDescriptorSetAllocateInfo alloc_ci{};
    alloc_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_ci.descriptorPool = pass_pool_;
    alloc_ci.descriptorSetCount = 3;
    alloc_ci.pSetLayouts = layouts.data();

    std::array<VkDescriptorSet, 3> sets{};
    if (vkAllocateDescriptorSets(device_, &alloc_ci, sets.data()) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate GTAO descriptor sets");
    }
    gtao_set_ = sets[0];
    blur_h_set_ = sets[1];
    blur_v_set_ = sets[2];

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
            throw std::runtime_error("Failed to create GTAO pipeline layout");
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

        if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipeline_ci,
                                      nullptr, &gtao_pipeline_) != VK_SUCCESS) {
            vkDestroyShaderModule(device_, comp_module, nullptr);
            throw std::runtime_error("Failed to create GTAO compute pipeline");
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
            throw std::runtime_error("Failed to create GTAO blur pipeline layout");
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

        if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipeline_ci,
                                      nullptr, &blur_pipeline_) != VK_SUCCESS) {
            vkDestroyShaderModule(device_, comp_module, nullptr);
            throw std::runtime_error("Failed to create GTAO blur compute pipeline");
        }
        vkDestroyShaderModule(device_, comp_module, nullptr);
    }

    // Create 1x1 placeholder images so binding 5 is valid from frame 1
    create_images(1, 1);

    FJELL_GFX_DEBUG("GTAO pass created");
}

void GtaoPass::destroy() {
    invalidate_imgui_texture();
    destroy_images();

    if (ao_sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, ao_sampler_, nullptr);
        ao_sampler_ = VK_NULL_HANDLE;
    }
    if (pass_pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, pass_pool_, nullptr);
        pass_pool_ = VK_NULL_HANDLE;
    }
    if (pass_set_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, pass_set_layout_, nullptr);
        pass_set_layout_ = VK_NULL_HANDLE;
    }
    if (gtao_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, gtao_pipeline_, nullptr);
        gtao_pipeline_ = VK_NULL_HANDLE;
    }
    if (gtao_layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, gtao_layout_, nullptr);
        gtao_layout_ = VK_NULL_HANDLE;
    }
    if (blur_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, blur_pipeline_, nullptr);
        blur_pipeline_ = VK_NULL_HANDLE;
    }
    if (blur_layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, blur_layout_, nullptr);
        blur_layout_ = VK_NULL_HANDLE;
    }
}

void GtaoPass::create_images(uint32_t width, uint32_t height) {
    extent_ = {width, height};

    auto create_image = [&](VkImage& image, VkDeviceMemory& memory, VkImageView& view) {
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

        if (vkCreateImage(device_, &ci, nullptr, &image) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create GTAO image");
        }

        VkMemoryRequirements mem_reqs;
        vkGetImageMemoryRequirements(device_, image, &mem_reqs);

        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = mem_reqs.size;
        alloc.memoryTypeIndex = Buffer::find_memory_type(
            physical_device_, mem_reqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(device_, &alloc, nullptr, &memory) != VK_SUCCESS) {
            throw std::runtime_error("Failed to allocate GTAO image memory");
        }
        vkBindImageMemory(device_, image, memory, 0);

        VkImageViewCreateInfo view_ci{};
        view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_ci.image = image;
        view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_ci.format = AO_FORMAT;
        view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        if (vkCreateImageView(device_, &view_ci, nullptr, &view) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create GTAO image view");
        }
    };

    create_image(ao_image_, ao_memory_, ao_view_);
    create_image(blur_image_, blur_memory_, blur_view_);

    images_need_init_ = true;
    update_descriptors();
}

void GtaoPass::destroy_images() {
    auto destroy_image = [&](VkImage& image, VkDeviceMemory& memory, VkImageView& view) {
        if (view != VK_NULL_HANDLE) { vkDestroyImageView(device_, view, nullptr); view = VK_NULL_HANDLE; }
        if (image != VK_NULL_HANDLE) { vkDestroyImage(device_, image, nullptr); image = VK_NULL_HANDLE; }
        if (memory != VK_NULL_HANDLE) { vkFreeMemory(device_, memory, nullptr); memory = VK_NULL_HANDLE; }
    };

    destroy_image(ao_image_, ao_memory_, ao_view_);
    destroy_image(blur_image_, blur_memory_, blur_view_);
}

void GtaoPass::resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;

    // AO images are half resolution
    uint32_t half_w = std::max(1u, width / 2);
    uint32_t half_h = std::max(1u, height / 2);
    if (extent_.width == half_w && extent_.height == half_h) return;

    vkDeviceWaitIdle(device_);
    invalidate_imgui_texture();
    destroy_images();
    create_images(half_w, half_h);
}

void GtaoPass::update_descriptors() {
    // gtao_set_: binding 0 = ao_image_ (storage write), binding 1 = unused
    {
        VkDescriptorImageInfo storage_info{};
        storage_info.imageView = ao_view_;
        storage_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = gtao_set_;
        write.dstBinding = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        write.descriptorCount = 1;
        write.pImageInfo = &storage_info;

        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }

    // blur_h_set_: binding 0 = blur_image_ (storage write), binding 1 = ao_image_ (sampled read)
    {
        VkDescriptorImageInfo storage_info{};
        storage_info.imageView = blur_view_;
        storage_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo sampled_info{};
        sampled_info.sampler = ao_sampler_;
        sampled_info.imageView = ao_view_;
        sampled_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        std::array<VkWriteDescriptorSet, 2> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = blur_h_set_;
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].descriptorCount = 1;
        writes[0].pImageInfo = &storage_info;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = blur_h_set_;
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo = &sampled_info;

        vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }

    // blur_v_set_: binding 0 = ao_image_ (storage write), binding 1 = blur_image_ (sampled read)
    {
        VkDescriptorImageInfo storage_info{};
        storage_info.imageView = ao_view_;
        storage_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo sampled_info{};
        sampled_info.sampler = ao_sampler_;
        sampled_info.imageView = blur_view_;
        sampled_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        std::array<VkWriteDescriptorSet, 2> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = blur_v_set_;
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].descriptorCount = 1;
        writes[0].pImageInfo = &storage_info;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = blur_v_set_;
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo = &sampled_info;

        vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
}

VkDescriptorSet GtaoPass::imgui_texture() {
    if (imgui_descriptor_ == VK_NULL_HANDLE && ao_view_ != VK_NULL_HANDLE) {
        imgui_descriptor_ = ImGui_ImplVulkan_AddTexture(
            ao_sampler_, ao_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return imgui_descriptor_;
}

void GtaoPass::invalidate_imgui_texture() {
    if (imgui_descriptor_ != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(imgui_descriptor_);
        imgui_descriptor_ = VK_NULL_HANDLE;
    }
}

void GtaoPass::dispatch(VkCommandBuffer cmd, VkDescriptorSet global_set,
                           uint32_t width, uint32_t height) {
    if (ao_view_ == VK_NULL_HANDLE) return;

    // Transition images from UNDEFINED on first use (after create or resize)
    if (images_need_init_) {
        std::array<VkImageMemoryBarrier2, 2> init_barriers{};

        // ao_image_: UNDEFINED → GENERAL (for compute storage write)
        init_barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        init_barriers[0].srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        init_barriers[0].srcAccessMask = 0;
        init_barriers[0].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        init_barriers[0].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        init_barriers[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        init_barriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        init_barriers[0].image = ao_image_;
        init_barriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        // blur_image_: UNDEFINED → GENERAL (for compute storage write)
        init_barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        init_barriers[1].srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        init_barriers[1].srcAccessMask = 0;
        init_barriers[1].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        init_barriers[1].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        init_barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        init_barriers[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        init_barriers[1].image = blur_image_;
        init_barriers[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        VkDependencyInfo init_dep{};
        init_dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        init_dep.imageMemoryBarrierCount = static_cast<uint32_t>(init_barriers.size());
        init_dep.pImageMemoryBarriers = init_barriers.data();
        vkCmdPipelineBarrier2(cmd, &init_dep);

        images_need_init_ = false;
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
        to_transfer.image = ao_image_;
        to_transfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &to_transfer;
        vkCmdPipelineBarrier2(cmd, &dep);

        VkClearColorValue clear_value{};
        clear_value.float32[0] = 1.0f;
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, ao_image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &clear_value, 1, &range);

        VkImageMemoryBarrier2 to_read{};
        to_read.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        to_read.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        to_read.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        to_read.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        to_read.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        to_read.image = ao_image_;
        to_read.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        dep.pImageMemoryBarriers = &to_read;
        vkCmdPipelineBarrier2(cmd, &dep);
        return;
    }

    // Compute at half resolution
    uint32_t half_w = extent_.width;
    uint32_t half_h = extent_.height;
    uint32_t groups_x = (half_w + TILE_SIZE - 1) / TILE_SIZE;
    uint32_t groups_y = (half_h + TILE_SIZE - 1) / TILE_SIZE;

    // ── Transition both images to GENERAL for compute write ─────────────
    // ao_image_ may be in SHADER_READ_ONLY from previous frame's final barrier.
    // blur_image_ may be in SHADER_READ_ONLY from previous frame's V blur read.
    // Using UNDEFINED as oldLayout discards contents (we overwrite everything).
    {
        std::array<VkImageMemoryBarrier2, 2> to_general{};

        to_general[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        to_general[0].srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        to_general[0].srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        to_general[0].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        to_general[0].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        to_general[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_general[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        to_general[0].image = ao_image_;
        to_general[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        to_general[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        to_general[1].srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        to_general[1].srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        to_general[1].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        to_general[1].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        to_general[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_general[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        to_general[1].image = blur_image_;
        to_general[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = static_cast<uint32_t>(to_general.size());
        dep.pImageMemoryBarriers = to_general.data();
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    // ── GTAO compute ────────────────────────────────────────────────
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gtao_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gtao_layout_,
                            0, 1, &global_set, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gtao_layout_,
                            1, 1, &gtao_set_, 0, nullptr);

    GtaoPushConstants push{};
    push.radius = radius_;
    push.intensity = intensity_;
    push.thickness = thickness_;
    push.max_pixel_radius = 128.0f; // half-res pixel budget
    push.viewport_width = half_w;
    push.viewport_height = half_h;
    push.full_viewport_width = width;
    push.full_viewport_height = height;

    vkCmdPushConstants(cmd, gtao_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);

    vkCmdDispatch(cmd, groups_x, groups_y, 1);

    // ── Barrier: GTAO write → blur H read ────────────────────────────
    VkImageMemoryBarrier2 ao_to_read{};
    ao_to_read.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    ao_to_read.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    ao_to_read.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    ao_to_read.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    ao_to_read.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    ao_to_read.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    ao_to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ao_to_read.image = ao_image_;
    ao_to_read.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &ao_to_read;
    vkCmdPipelineBarrier2(cmd, &dep);

    // ── Blur horizontal ────────────────────────────────────────────────
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, blur_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, blur_layout_,
                            0, 1, &global_set, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, blur_layout_,
                            1, 1, &blur_h_set_, 0, nullptr);

    BlurPushConstants blur_push{};
    blur_push.viewport_width = half_w;
    blur_push.viewport_height = half_h;
    blur_push.direction = 0;
    blur_push.depth_threshold = 0.1f;

    vkCmdPushConstants(cmd, blur_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(blur_push), &blur_push);

    vkCmdDispatch(cmd, groups_x, groups_y, 1);

    // ── Barrier: blur H write → blur V read (blur_image_ to read, ao_image_ to write) ──
    std::array<VkImageMemoryBarrier2, 2> blur_barriers{};

    // blur_image_: GENERAL → SHADER_READ_ONLY (for blur V read)
    blur_barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    blur_barriers[0].srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    blur_barriers[0].srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    blur_barriers[0].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    blur_barriers[0].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    blur_barriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    blur_barriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    blur_barriers[0].image = blur_image_;
    blur_barriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    // ao_image_: SHADER_READ_ONLY → GENERAL (for blur V write)
    blur_barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    blur_barriers[1].srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    blur_barriers[1].srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    blur_barriers[1].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    blur_barriers[1].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    blur_barriers[1].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    blur_barriers[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    blur_barriers[1].image = ao_image_;
    blur_barriers[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkDependencyInfo blur_dep{};
    blur_dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    blur_dep.imageMemoryBarrierCount = static_cast<uint32_t>(blur_barriers.size());
    blur_dep.pImageMemoryBarriers = blur_barriers.data();
    vkCmdPipelineBarrier2(cmd, &blur_dep);

    // ── Blur vertical ──────────────────────────────────────────────────
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, blur_layout_,
                            1, 1, &blur_v_set_, 0, nullptr);

    blur_push.direction = 1;
    vkCmdPushConstants(cmd, blur_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(blur_push), &blur_push);

    vkCmdDispatch(cmd, groups_x, groups_y, 1);

    // ── Final barrier: ao_image_ GENERAL → SHADER_READ_ONLY for mesh.frag ──
    VkImageMemoryBarrier2 ao_final{};
    ao_final.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    ao_final.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    ao_final.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    ao_final.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    ao_final.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    ao_final.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    ao_final.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ao_final.image = ao_image_;
    ao_final.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkDependencyInfo final_dep{};
    final_dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    final_dep.imageMemoryBarrierCount = 1;
    final_dep.pImageMemoryBarriers = &ao_final;
    vkCmdPipelineBarrier2(cmd, &final_dep);
}

} // namespace fjell
