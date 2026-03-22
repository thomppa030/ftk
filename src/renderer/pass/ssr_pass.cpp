#include "renderer/pass/ssr_pass.hpp"
#include "renderer/device.hpp"
#include "renderer/offscreen_pass.hpp"
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

static constexpr uint32_t TILE_SIZE = 8;
static constexpr VkFormat HIZ_FORMAT = VK_FORMAT_R32_SFLOAT;
static constexpr VkFormat SSR_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;

struct HizPushConstants {
    uint32_t src_width;
    uint32_t src_height;
    uint32_t dst_width;
    uint32_t dst_height;
};

struct SsrTracePushConstants {
    uint32_t viewport_width;
    uint32_t viewport_height;
    uint32_t hiz_mip_count;
    uint32_t max_steps;
    float max_distance;
    float thickness;
    float intensity;
    float pad;
};

struct SsrCompositePushConstants {
    uint32_t viewport_width;
    uint32_t viewport_height;
    float intensity;
    float pad;
};

// ── Helpers ──────────────────────────────────────────────────────────────

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

    VkPipeline pipeline{VK_NULL_HANDLE};
    if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline) != VK_SUCCESS) {
        vkDestroyShaderModule(device, module, nullptr);
        throw std::runtime_error("Failed to create compute pipeline: " + shader_path);
    }
    vkDestroyShaderModule(device, module, nullptr);
    return pipeline;
}

static void insert_image_barrier(VkCommandBuffer cmd, VkImage image,
                                  VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                                  VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access,
                                  VkImageLayout old_layout, VkImageLayout new_layout,
                                  uint32_t base_mip = 0, uint32_t mip_count = 1) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, base_mip, mip_count, 0, 1};

    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

// ── Create ───────────────────────────────────────────────────────────────

void SsrPass::create(Device& device, const PassResources& resources) {
    device_ = device.handle();
    physical_device_ = device.physical_device();
    auto global_set_layout = resources.global_set_layout;

    // ── Samplers ─────────────────────────────────────────────────────
    {
        VkSamplerCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        ci.magFilter = VK_FILTER_NEAREST;
        ci.minFilter = VK_FILTER_NEAREST;
        ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.maxLod = static_cast<float>(MAX_HIZ_MIPS);
        if (vkCreateSampler(device_, &ci, nullptr, &nearest_sampler_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create SSR nearest sampler");
        }
    }
    {
        VkSamplerCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        ci.magFilter = VK_FILTER_LINEAR;
        ci.minFilter = VK_FILTER_LINEAR;
        ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(device_, &ci, nullptr, &linear_sampler_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create SSR linear sampler");
        }
    }

    // ── Hi-Z set layout: binding 0 = sampler (read), binding 1 = storage image (write) ──
    {
        std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
        bindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

        VkDescriptorSetLayoutCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ci.bindingCount = static_cast<uint32_t>(bindings.size());
        ci.pBindings = bindings.data();
        if (vkCreateDescriptorSetLayout(device_, &ci, nullptr, &hiz_set_layout_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create Hi-Z descriptor set layout");
        }
    }

    // ── Trace set layout: binding 0 = hiz, binding 1 = prev_color, binding 2 = ssr output ──
    {
        std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
        bindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

        VkDescriptorSetLayoutCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ci.bindingCount = static_cast<uint32_t>(bindings.size());
        ci.pBindings = bindings.data();
        if (vkCreateDescriptorSetLayout(device_, &ci, nullptr, &trace_set_layout_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create SSR trace descriptor set layout");
        }
    }

    // ── Composite set layout: binding 0 = ssr (read), binding 1 = scene color (read/write) ──
    {
        std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
        bindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

        VkDescriptorSetLayoutCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ci.bindingCount = static_cast<uint32_t>(bindings.size());
        ci.pBindings = bindings.data();
        if (vkCreateDescriptorSetLayout(device_, &ci, nullptr, &composite_set_layout_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create SSR composite descriptor set layout");
        }
    }

    // ── Pipeline layouts ────────────────────────────────────────────
    auto create_layout = [&](VkDescriptorSetLayout pass_layout, uint32_t push_size) {
        std::array<VkDescriptorSetLayout, 2> sets = {global_set_layout, pass_layout};
        VkPushConstantRange push{};
        push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push.size = push_size;

        VkPipelineLayoutCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        ci.setLayoutCount = static_cast<uint32_t>(sets.size());
        ci.pSetLayouts = sets.data();
        ci.pushConstantRangeCount = 1;
        ci.pPushConstantRanges = &push;

        VkPipelineLayout layout{VK_NULL_HANDLE};
        if (vkCreatePipelineLayout(device_, &ci, nullptr, &layout) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create SSR pipeline layout");
        }
        return layout;
    };

    hiz_pipeline_layout_ = create_layout(hiz_set_layout_, sizeof(HizPushConstants));
    trace_pipeline_layout_ = create_layout(trace_set_layout_, sizeof(SsrTracePushConstants));
    composite_pipeline_layout_ = create_layout(composite_set_layout_, sizeof(SsrCompositePushConstants));

    // ── Compute pipelines ───────────────────────────────────────────
    hiz_pipeline_ = create_compute_pipeline(device_, hiz_pipeline_layout_, "shaders/ssr_hiz_build.comp.spv");
    trace_pipeline_ = create_compute_pipeline(device_, trace_pipeline_layout_, "shaders/ssr_trace.comp.spv");
    composite_pipeline_ = create_compute_pipeline(device_, composite_pipeline_layout_, "shaders/ssr_composite.comp.spv");

    // ── Descriptor pools ────────────────────────────────────────────
    {
        std::array<VkDescriptorPoolSize, 2> sizes{};
        sizes[0] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_HIZ_MIPS};
        sizes[1] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, MAX_HIZ_MIPS};

        VkDescriptorPoolCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        ci.maxSets = MAX_HIZ_MIPS;
        ci.poolSizeCount = static_cast<uint32_t>(sizes.size());
        ci.pPoolSizes = sizes.data();
        if (vkCreateDescriptorPool(device_, &ci, nullptr, &hiz_pool_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create Hi-Z descriptor pool");
        }
    }
    {
        std::array<VkDescriptorPoolSize, 2> sizes{};
        sizes[0] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
        sizes[1] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1};

        VkDescriptorPoolCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        ci.maxSets = 1;
        ci.poolSizeCount = static_cast<uint32_t>(sizes.size());
        ci.pPoolSizes = sizes.data();
        if (vkCreateDescriptorPool(device_, &ci, nullptr, &trace_pool_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create SSR trace descriptor pool");
        }
    }
    {
        std::array<VkDescriptorPoolSize, 2> sizes{};
        sizes[0] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
        sizes[1] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1};

        VkDescriptorPoolCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        ci.maxSets = 1;
        ci.poolSizeCount = static_cast<uint32_t>(sizes.size());
        ci.pPoolSizes = sizes.data();
        if (vkCreateDescriptorPool(device_, &ci, nullptr, &composite_pool_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create SSR composite descriptor pool");
        }
    }

    // Allocate trace + composite sets (hiz sets allocated in create_images)
    {
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = trace_pool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &trace_set_layout_;
        if (vkAllocateDescriptorSets(device_, &ai, &trace_set_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to allocate SSR trace descriptor set");
        }
    }
    {
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = composite_pool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &composite_set_layout_;
        if (vkAllocateDescriptorSets(device_, &ai, &composite_set_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to allocate SSR composite descriptor set");
        }
    }

    // Create 1x1 placeholder so descriptors are valid from frame 1
    create_images(1, 1);

    FJELL_GFX_DEBUG("SSR pass created");
}

// ── Destroy ──────────────────────────────────────────────────────────────

void SsrPass::destroy() {
    invalidate_imgui_texture();
    destroy_images();

    auto safe_destroy = [&](auto destroy_fn, auto& handle) {
        if (handle != VK_NULL_HANDLE) {
            destroy_fn(device_, handle, nullptr);
            handle = VK_NULL_HANDLE;
        }
    };

    safe_destroy(vkDestroySampler, nearest_sampler_);
    safe_destroy(vkDestroySampler, linear_sampler_);
    safe_destroy(vkDestroyDescriptorPool, hiz_pool_);
    safe_destroy(vkDestroyDescriptorPool, trace_pool_);
    safe_destroy(vkDestroyDescriptorPool, composite_pool_);
    safe_destroy(vkDestroyDescriptorSetLayout, hiz_set_layout_);
    safe_destroy(vkDestroyDescriptorSetLayout, trace_set_layout_);
    safe_destroy(vkDestroyDescriptorSetLayout, composite_set_layout_);
    safe_destroy(vkDestroyPipeline, hiz_pipeline_);
    safe_destroy(vkDestroyPipelineLayout, hiz_pipeline_layout_);
    safe_destroy(vkDestroyPipeline, trace_pipeline_);
    safe_destroy(vkDestroyPipelineLayout, trace_pipeline_layout_);
    safe_destroy(vkDestroyPipeline, composite_pipeline_);
    safe_destroy(vkDestroyPipelineLayout, composite_pipeline_layout_);
}

// ── Image Management ────────────────────────────────────────────────────

void SsrPass::create_images(uint32_t width, uint32_t height) {
    extent_ = {width, height};

    hiz_mip_count_ = static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1;
    hiz_mip_count_ = std::min(hiz_mip_count_, MAX_HIZ_MIPS);

    // ── Hi-Z image ──────────────────────────────────────────────────
    {
        VkImageCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = HIZ_FORMAT;
        ci.extent = {width, height, 1};
        ci.mipLevels = hiz_mip_count_;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(device_, &ci, nullptr, &hiz_image_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create Hi-Z image");
        }

        VkMemoryRequirements reqs;
        vkGetImageMemoryRequirements(device_, hiz_image_, &reqs);

        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = reqs.size;
        alloc.memoryTypeIndex = Buffer::find_memory_type(
            physical_device_, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(device_, &alloc, nullptr, &hiz_memory_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to allocate Hi-Z image memory");
        }
        vkBindImageMemory(device_, hiz_image_, hiz_memory_, 0);

        for (uint32_t i = 0; i < hiz_mip_count_; ++i) {
            VkImageViewCreateInfo vi{};
            vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vi.image = hiz_image_;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = HIZ_FORMAT;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, i, 1, 0, 1};
            if (vkCreateImageView(device_, &vi, nullptr, &hiz_mip_views_[i]) != VK_SUCCESS) {
                throw std::runtime_error("Failed to create Hi-Z mip view");
            }
        }

        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = hiz_image_;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = HIZ_FORMAT;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, hiz_mip_count_, 0, 1};
        if (vkCreateImageView(device_, &vi, nullptr, &hiz_sampled_view_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create Hi-Z sampled view");
        }
    }

    // ── SSR output ──────────────────────────────────────────────────
    {
        VkImageCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = SSR_FORMAT;
        ci.extent = {width, height, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                   VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(device_, &ci, nullptr, &ssr_image_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create SSR output image");
        }

        VkMemoryRequirements reqs;
        vkGetImageMemoryRequirements(device_, ssr_image_, &reqs);

        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = reqs.size;
        alloc.memoryTypeIndex = Buffer::find_memory_type(
            physical_device_, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(device_, &alloc, nullptr, &ssr_memory_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to allocate SSR output image memory");
        }
        vkBindImageMemory(device_, ssr_image_, ssr_memory_, 0);

        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = ssr_image_;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = SSR_FORMAT;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device_, &vi, nullptr, &ssr_view_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create SSR output image view");
        }
    }

    images_need_init_ = true;
    last_depth_view_ = VK_NULL_HANDLE;
    last_prev_color_view_ = VK_NULL_HANDLE;
    last_color_view_ = VK_NULL_HANDLE;

    // ── Allocate Hi-Z descriptor sets ───────────────────────────────
    vkResetDescriptorPool(device_, hiz_pool_, 0);
    for (uint32_t i = 0; i < hiz_mip_count_; ++i) {
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = hiz_pool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &hiz_set_layout_;
        if (vkAllocateDescriptorSets(device_, &ai, &hiz_sets_[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to allocate Hi-Z descriptor set");
        }
    }

    // Write dst (storage) binding for each Hi-Z mip — static, doesn't change
    for (uint32_t i = 0; i < hiz_mip_count_; ++i) {
        VkDescriptorImageInfo dst_info{};
        dst_info.imageView = hiz_mip_views_[i];
        dst_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = hiz_sets_[i];
        write.dstBinding = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        write.descriptorCount = 1;
        write.pImageInfo = &dst_info;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }

    // Write src binding for Hi-Z mips 1+ (reads previous mip)
    // Mip 0 src is the depth texture — updated per-frame in dispatch
    for (uint32_t i = 1; i < hiz_mip_count_; ++i) {
        VkDescriptorImageInfo src_info{};
        src_info.sampler = nearest_sampler_;
        src_info.imageView = hiz_mip_views_[i - 1];
        src_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = hiz_sets_[i];
        write.dstBinding = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &src_info;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }

    // Write trace set: binding 0 = hiz, binding 2 = ssr output (binding 1 = prev_color, per-frame)
    {
        VkDescriptorImageInfo hiz_info{};
        hiz_info.sampler = nearest_sampler_;
        hiz_info.imageView = hiz_sampled_view_;
        hiz_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo ssr_info{};
        ssr_info.imageView = ssr_view_;
        ssr_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        std::array<VkWriteDescriptorSet, 2> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = trace_set_;
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].descriptorCount = 1;
        writes[0].pImageInfo = &hiz_info;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = trace_set_;
        writes[1].dstBinding = 2;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo = &ssr_info;

        vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }

    // Write composite set: binding 0 = ssr (binding 1 = scene color, per-frame)
    {
        VkDescriptorImageInfo ssr_info{};
        ssr_info.sampler = nearest_sampler_;
        ssr_info.imageView = ssr_view_;
        ssr_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = composite_set_;
        write.dstBinding = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &ssr_info;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }
}

void SsrPass::destroy_images() {
    for (auto& view : hiz_mip_views_) {
        if (view != VK_NULL_HANDLE) {
            vkDestroyImageView(device_, view, nullptr);
            view = VK_NULL_HANDLE;
        }
    }
    if (hiz_sampled_view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, hiz_sampled_view_, nullptr);
        hiz_sampled_view_ = VK_NULL_HANDLE;
    }
    if (hiz_image_ != VK_NULL_HANDLE) { vkDestroyImage(device_, hiz_image_, nullptr); hiz_image_ = VK_NULL_HANDLE; }
    if (hiz_memory_ != VK_NULL_HANDLE) { vkFreeMemory(device_, hiz_memory_, nullptr); hiz_memory_ = VK_NULL_HANDLE; }
    if (ssr_view_ != VK_NULL_HANDLE) { vkDestroyImageView(device_, ssr_view_, nullptr); ssr_view_ = VK_NULL_HANDLE; }
    if (ssr_image_ != VK_NULL_HANDLE) { vkDestroyImage(device_, ssr_image_, nullptr); ssr_image_ = VK_NULL_HANDLE; }
    if (ssr_memory_ != VK_NULL_HANDLE) { vkFreeMemory(device_, ssr_memory_, nullptr); ssr_memory_ = VK_NULL_HANDLE; }
}

void SsrPass::resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;
    if (extent_.width == width && extent_.height == height) return;

    vkDeviceWaitIdle(device_);
    invalidate_imgui_texture();
    destroy_images();
    create_images(width, height);
}

// ── ImGui ───────────────────────────────────────────────────────────────

VkDescriptorSet SsrPass::imgui_texture() {
    if (imgui_descriptor_ == VK_NULL_HANDLE && ssr_view_ != VK_NULL_HANDLE) {
        imgui_descriptor_ = ImGui_ImplVulkan_AddTexture(
            linear_sampler_, ssr_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return imgui_descriptor_;
}

void SsrPass::invalidate_imgui_texture() {
    if (imgui_descriptor_ != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(imgui_descriptor_);
        imgui_descriptor_ = VK_NULL_HANDLE;
    }
}

// ── Dispatch ────────────────────────────────────────────────────────────

void SsrPass::dispatch(VkCommandBuffer cmd, VkDescriptorSet global_set,
                       VkImageView depth_view, VkImageView color_view,
                       VkImageView prev_color_view,
                       uint32_t width, uint32_t height) {
    if (hiz_image_ == VK_NULL_HANDLE || ssr_image_ == VK_NULL_HANDLE) return;
    if (prev_color_view == VK_NULL_HANDLE) return;

    if (!enabled_) {
        // Clear SSR to black
        insert_image_barrier(cmd, ssr_image_,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        VkClearColorValue clear{};
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, ssr_image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);

        insert_image_barrier(cmd, ssr_image_,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        return;
    }

    uint32_t groups_x = (width + TILE_SIZE - 1) / TILE_SIZE;
    uint32_t groups_y = (height + TILE_SIZE - 1) / TILE_SIZE;

    // ── Update per-frame descriptors ────────────────────────────────

    // Hi-Z mip 0: bind depth texture as source
    if (depth_view != last_depth_view_) {
        last_depth_view_ = depth_view;

        VkDescriptorImageInfo src_info{};
        src_info.sampler = nearest_sampler_;
        src_info.imageView = depth_view;
        src_info.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = hiz_sets_[0];
        write.dstBinding = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &src_info;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }

    // Trace binding 1: prev frame color
    if (prev_color_view != last_prev_color_view_) {
        last_prev_color_view_ = prev_color_view;

        VkDescriptorImageInfo info{};
        info.sampler = linear_sampler_;
        info.imageView = prev_color_view;
        info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = trace_set_;
        write.dstBinding = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &info;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }

    // Composite binding 1: scene color (storage image, read/write)
    if (color_view != last_color_view_) {
        last_color_view_ = color_view;

        VkDescriptorImageInfo info{};
        info.imageView = color_view;
        info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = composite_set_;
        write.dstBinding = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        write.descriptorCount = 1;
        write.pImageInfo = &info;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }

    // ── Transition Hi-Z mips to GENERAL for writing ────────────────
    // On first frame: UNDEFINED → GENERAL. On subsequent frames: SHADER_READ_ONLY → GENERAL.
    // Using UNDEFINED as oldLayout discards contents which is fine since we overwrite every mip.
    insert_image_barrier(cmd, hiz_image_,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
        0, hiz_mip_count_);

    images_need_init_ = false;

    // ═══════════════════════════════════════════════════════════════════
    // Pass 1: Hi-Z Build (one dispatch per mip)
    // ═══════════════════════════════════════════════════════════════════

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hiz_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hiz_pipeline_layout_,
                            0, 1, &global_set, 0, nullptr);

    uint32_t src_w = width;
    uint32_t src_h = height;

    for (uint32_t mip = 0; mip < hiz_mip_count_; ++mip) {
        uint32_t dst_w = std::max(1u, width >> mip);
        uint32_t dst_h = std::max(1u, height >> mip);

        // For mip 0: src is depth texture (bound in hiz_sets_[0] binding 0 above)
        //            dst is hiz mip 0 (already in hiz_sets_[0] binding 1)
        //            src dimensions = depth texture size = width x height
        //            dst dimensions = same (mip 0 = full res)
        //            This effectively copies depth → hiz mip 0 (with max of 2x2 = identity for same size)
        //
        // Wait — mip 0 at full res, reading 2x2 from a same-size source, just picks max of 4 neighbors.
        // That's not a copy — it blurs. For mip 0 we want an exact copy.
        // Actually for the hi-z to work, mip 0 should just be the depth values.
        // The shader reads coord*2 from source, but if src and dst are the same size,
        // coord*2 goes out of bounds. We need mip 0 = depth copy.
        //
        // Better approach: don't build mip 0 with the downsample shader.
        // Start building from mip 1 (half-res) reading from depth (full-res).
        // The trace shader reads depth directly from global binding 4 at level 0,
        // and from hiz_texture at levels 1+.
        // But hiz_texture is a single image with all mips — mip 0 would be garbage.
        //
        // Simplest fix: have mip 0 of hiz be the depth, copied via the shader
        // with src_width = dst_width (no downscale). The shader reads texelFetch
        // at coord*2 which doubles the coord — that's wrong for same-size.
        //
        // Let's just skip mip 0 and start the hiz pyramid at mip 1.
        // In the trace shader, use depth texture (global binding 4) for level 0
        // and hiz for levels 1+. But textureLod on hiz at level 0 would still
        // sample the (garbage) mip 0.
        //
        // Clean solution: just do the copy. Write a mip 0 dispatch that does
        // imageStore(dst, coord, texelFetch(src, coord, 0)). But the existing
        // shader always does coord*2...
        //
        // Easiest: skip mip 0 entirely. Don't use it. Start Hi-Z at mip 1.
        // The hiz mip 0 will be unused/garbage.
        // In the trace loop, start cur_level at 1 (minimum), never go below 1.
        // At "level 0" hit testing, use the depth texture directly.
        //
        // Actually this makes the trace shader more complex. Let me just fix
        // the hiz build shader to handle the mip 0 case (identity copy).
        // We can use the push constants: if src_width == dst_width, the shader
        // just does texelFetch(src, coord, 0) without the 2x downscale.
        //
        // OR — even simpler — for mip 0, set src dimensions to width*2 and
        // dst to width. Then coord*2 maps to the right range in the depth texture.
        // No wait, depth is width x height, so coord*2 at mip 0 (also width x height)
        // goes to 2*width which is OOB.
        //
        // OK let me just special-case: for mip 0 use a trivial copy approach.
        // The hiz build reads texelFetch(src, coord*2, 0). For a depth texture
        // that's width x height, and mip 0 output is also width x height,
        // coord*2 at pixel (width/2, height/2) = (width, height) which is OOB.
        // The shader clamps to src_width-1/src_height-1, so it reads the edge.
        // Half the pixels will read clamped values — that's wrong.
        //
        // Final approach: mip 0 is half-res (width/2 x height/2), not full-res.
        // This changes the hiz pyramid: mip 0 = w/2 x h/2, mip 1 = w/4 x h/4, etc.
        // The trace shader reads depth at level 0 from the original depth texture
        // (global binding 4) and reads hiz at levels 1+ (where level 1 = hiz mip 0).
        // This way the hiz build is always a proper 2x downsample.

        if (mip == 0) {
            // Mip 0 of hiz = half-res downsample from depth texture
            dst_w = std::max(1u, width / 2);
            dst_h = std::max(1u, height / 2);
        } else {
            // Subsequent mips halve from previous
            uint32_t prev_w = std::max(1u, width >> (mip - 1)) ;
            uint32_t prev_h = std::max(1u, height >> (mip - 1));
            if (mip == 1) {
                // mip 1 reads from hiz mip 0 which is w/2 x h/2
                prev_w = std::max(1u, width / 2);
                prev_h = std::max(1u, height / 2);
            }
            src_w = prev_w;
            src_h = prev_h;
            dst_w = std::max(1u, prev_w / 2);
            dst_h = std::max(1u, prev_h / 2);
        }

        // Transition this mip to GENERAL for write (if not already)
        if (mip > 0) {
            // Previous mip: GENERAL → SHADER_READ_ONLY (was just written)
            // Already done at end of previous iteration
        }

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hiz_pipeline_layout_,
                                1, 1, &hiz_sets_[mip], 0, nullptr);

        HizPushConstants push{};
        push.src_width = (mip == 0) ? width : src_w;
        push.src_height = (mip == 0) ? height : src_h;
        push.dst_width = dst_w;
        push.dst_height = dst_h;

        vkCmdPushConstants(cmd, hiz_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(push), &push);

        uint32_t gx = (dst_w + TILE_SIZE - 1) / TILE_SIZE;
        uint32_t gy = (dst_h + TILE_SIZE - 1) / TILE_SIZE;
        vkCmdDispatch(cmd, gx, gy, 1);

        // Barrier: mip write complete → ready for next mip read (or trace read)
        insert_image_barrier(cmd, hiz_image_,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            mip, 1);
    }

    // SSR output → GENERAL for write
    insert_image_barrier(cmd, ssr_image_,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

    // ═══════════════════════════════════════════════════════════════════
    // Pass 2: SSR Trace
    // ═══════════════════════════════════════════════════════════════════

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, trace_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, trace_pipeline_layout_,
                            0, 1, &global_set, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, trace_pipeline_layout_,
                            1, 1, &trace_set_, 0, nullptr);

    SsrTracePushConstants trace_push{};
    trace_push.viewport_width = width;
    trace_push.viewport_height = height;
    trace_push.hiz_mip_count = hiz_mip_count_;
    trace_push.max_steps = max_steps_;
    trace_push.max_distance = max_distance_;
    trace_push.thickness = thickness_;
    trace_push.intensity = intensity_;

    vkCmdPushConstants(cmd, trace_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(trace_push), &trace_push);
    vkCmdDispatch(cmd, groups_x, groups_y, 1);

    // Barrier: SSR write → composite read
    insert_image_barrier(cmd, ssr_image_,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // ═══════════════════════════════════════════════════════════════════
    // Pass 3: SSR Composite (reads SSR, reads+writes scene color)
    // ═══════════════════════════════════════════════════════════════════

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, composite_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, composite_pipeline_layout_,
                            0, 1, &global_set, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, composite_pipeline_layout_,
                            1, 1, &composite_set_, 0, nullptr);

    SsrCompositePushConstants comp_push{};
    comp_push.viewport_width = width;
    comp_push.viewport_height = height;
    comp_push.intensity = intensity_;

    vkCmdPushConstants(cmd, composite_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(comp_push), &comp_push);
    vkCmdDispatch(cmd, groups_x, groups_y, 1);
}

void SsrPass::record(const FrameContext& ctx) {
    auto extent = ctx.target->extent();
    dispatch(ctx.cmd, ctx.global_descriptor_set,
             ctx.target->depth_view(), ctx.target->color_view(),
             ctx.prev_frame_color_view,
             extent.width, extent.height);
}

} // namespace fjell
