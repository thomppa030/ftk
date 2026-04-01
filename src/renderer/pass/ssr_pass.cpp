#include "renderer/pass/ssr_pass.hpp"
#include "renderer/device.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/buffer.hpp"
#include "renderer/shader_utils.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#include <imgui_impl_vulkan.h>

#include <array>
#include <stdexcept>

namespace fjell {

static constexpr uint32_t TILE_SIZE = 8;
static constexpr VkFormat SSR_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;

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

// ── Images (per-viewport state) ─────────────────────────────────────────────

void SsrPass::Images::create(VkDevice device, VkPhysicalDevice physical_device,
                               VkDescriptorSetLayout trace_layout,
                               VkDescriptorSetLayout composite_layout,
                               VkSampler nearest_sampler,
                               uint32_t width, uint32_t height) {
    extent = {width, height};

    // SSR output image
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

        if (vkCreateImage(device, &ci, nullptr, &ssr_image) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create SSR output image");
        }

        VkMemoryRequirements reqs;
        vkGetImageMemoryRequirements(device, ssr_image, &reqs);

        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = reqs.size;
        alloc.memoryTypeIndex = Buffer::find_memory_type(
            physical_device, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(device, &alloc, nullptr, &ssr_memory) != VK_SUCCESS) {
            throw std::runtime_error("Failed to allocate SSR output image memory");
        }
        vkBindImageMemory(device, ssr_image, ssr_memory, 0);

        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = ssr_image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = SSR_FORMAT;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device, &vi, nullptr, &ssr_view) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create SSR output image view");
        }
    }

    // Own descriptor pool: 2 sets (trace + composite)
    // trace: 2 combined_image_sampler + 1 storage_image
    // composite: 2 combined_image_sampler + 1 storage_image
    {
        std::array<VkDescriptorPoolSize, 2> sizes{};
        sizes[0] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4};
        sizes[1] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2};

        VkDescriptorPoolCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        ci.maxSets = 2;
        ci.poolSizeCount = static_cast<uint32_t>(sizes.size());
        ci.pPoolSizes = sizes.data();
        if (vkCreateDescriptorPool(device, &ci, nullptr, &pool) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create SSR images descriptor pool");
        }
    }

    // Allocate sets
    {
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &trace_layout;
        if (vkAllocateDescriptorSets(device, &ai, &trace_set) != VK_SUCCESS) {
            throw std::runtime_error("Failed to allocate SSR trace descriptor set");
        }
    }
    {
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &composite_layout;
        if (vkAllocateDescriptorSets(device, &ai, &composite_set) != VK_SUCCESS) {
            throw std::runtime_error("Failed to allocate SSR composite descriptor set");
        }
    }

    // Reset cached views
    last_hiz_view = VK_NULL_HANDLE;
    last_prev_color_view = VK_NULL_HANDLE;
    last_color_view = VK_NULL_HANDLE;
    last_mr_view = VK_NULL_HANDLE;

    // Write trace set: binding 2 = ssr output (bindings 0,1 are per-frame)
    {
        VkDescriptorImageInfo ssr_info{};
        ssr_info.imageView = ssr_view;
        ssr_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = trace_set;
        write.dstBinding = 2;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        write.descriptorCount = 1;
        write.pImageInfo = &ssr_info;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }

    // Write composite set: binding 0 = ssr (binding 1 = scene color, per-frame)
    {
        VkDescriptorImageInfo ssr_info{};
        ssr_info.sampler = nearest_sampler;
        ssr_info.imageView = ssr_view;
        ssr_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = composite_set;
        write.dstBinding = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &ssr_info;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }
}

void SsrPass::Images::destroy(VkDevice device) {
    invalidate_imgui_texture();
    if (pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, pool, nullptr);
        pool = VK_NULL_HANDLE;
        trace_set = VK_NULL_HANDLE;
        composite_set = VK_NULL_HANDLE;
    }
    if (ssr_view != VK_NULL_HANDLE) { vkDestroyImageView(device, ssr_view, nullptr); ssr_view = VK_NULL_HANDLE; }
    if (ssr_image != VK_NULL_HANDLE) { vkDestroyImage(device, ssr_image, nullptr); ssr_image = VK_NULL_HANDLE; }
    if (ssr_memory != VK_NULL_HANDLE) { vkFreeMemory(device, ssr_memory, nullptr); ssr_memory = VK_NULL_HANDLE; }
    extent = {};
    last_hiz_view = VK_NULL_HANDLE;
    last_prev_color_view = VK_NULL_HANDLE;
    last_color_view = VK_NULL_HANDLE;
    last_mr_view = VK_NULL_HANDLE;
}

void SsrPass::Images::resize(VkDevice device, VkPhysicalDevice physical_device,
                               VkDescriptorSetLayout trace_layout,
                               VkDescriptorSetLayout composite_layout,
                               VkSampler nearest_sampler,
                               uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;
    if (extent.width == width && extent.height == height) return;
    vkDeviceWaitIdle(device);

    destroy(device);
    create(device, physical_device, trace_layout, composite_layout, nearest_sampler, width, height);
}

VkDescriptorSet SsrPass::Images::imgui_texture(VkSampler sampler) {
    if (imgui_descriptor == VK_NULL_HANDLE && ssr_view != VK_NULL_HANDLE) {
        imgui_descriptor = ImGui_ImplVulkan_AddTexture(
            sampler, ssr_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return imgui_descriptor;
}

void SsrPass::Images::invalidate_imgui_texture() {
    if (imgui_descriptor != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(imgui_descriptor);
        imgui_descriptor = VK_NULL_HANDLE;
    }
}

// ── SsrPass (singleton) ─────────────────────────────────────────────────────

Result<> SsrPass::create(Device& device, const PassResources& resources) {
    device_ = device.handle();
    physical_device_ = device.physical_device();
    auto global_set_layout = resources.global_set_layout;

    // Samplers
    {
        VkSamplerCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        ci.magFilter = VK_FILTER_NEAREST;
        ci.minFilter = VK_FILTER_NEAREST;
        ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(device_, &ci, nullptr, &nearest_sampler_) != VK_SUCCESS) {
            return make_error("Failed to create SSR nearest sampler");
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
            return make_error("Failed to create SSR linear sampler");
        }
    }

    // Trace set layout: binding 0 = hiz, binding 1 = prev_color, binding 2 = ssr output
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
            return make_error("Failed to create SSR trace descriptor set layout");
        }
    }

    // Composite set layout: binding 0 = ssr (read), binding 1 = scene color (read/write), binding 2 = MR (read)
    {
        std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
        bindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[2] = {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

        VkDescriptorSetLayoutCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ci.bindingCount = static_cast<uint32_t>(bindings.size());
        ci.pBindings = bindings.data();
        if (vkCreateDescriptorSetLayout(device_, &ci, nullptr, &composite_set_layout_) != VK_SUCCESS) {
            return make_error("Failed to create SSR composite descriptor set layout");
        }
    }

    // Pipeline layouts
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

    trace_pipeline_layout_ = create_layout(trace_set_layout_, sizeof(SsrTracePushConstants));
    composite_pipeline_layout_ = create_layout(composite_set_layout_, sizeof(SsrCompositePushConstants));

    // Compute pipelines
    trace_pipeline_ = create_compute_pipeline(device_, trace_pipeline_layout_, "shaders/ssr_trace.comp.spv");
    composite_pipeline_ = create_compute_pipeline(device_, composite_pipeline_layout_, "shaders/ssr_composite.comp.spv");

    // Create 1x1 placeholder so descriptors are valid from frame 1
    default_images_.create(device_, physical_device_, trace_set_layout_, composite_set_layout_, nearest_sampler_, 1, 1);

    FJELL_GFX_DEBUG("SSR pass created");
    return {};
}

void SsrPass::destroy() {
    default_images_.destroy(device_);

    auto safe_destroy = [&](auto destroy_fn, auto& handle) {
        if (handle != VK_NULL_HANDLE) {
            destroy_fn(device_, handle, nullptr);
            handle = VK_NULL_HANDLE;
        }
    };

    safe_destroy(vkDestroySampler, nearest_sampler_);
    safe_destroy(vkDestroySampler, linear_sampler_);
    safe_destroy(vkDestroyDescriptorSetLayout, trace_set_layout_);
    safe_destroy(vkDestroyDescriptorSetLayout, composite_set_layout_);
    safe_destroy(vkDestroyPipeline, trace_pipeline_);
    safe_destroy(vkDestroyPipelineLayout, trace_pipeline_layout_);
    safe_destroy(vkDestroyPipeline, composite_pipeline_);
    safe_destroy(vkDestroyPipelineLayout, composite_pipeline_layout_);
}

void SsrPass::resize(uint32_t width, uint32_t height) {
    default_images_.resize(device_, physical_device_, trace_set_layout_, composite_set_layout_, nearest_sampler_, width, height);
}

VkDescriptorSet SsrPass::imgui_texture() {
    return default_images_.imgui_texture(linear_sampler_);
}

void SsrPass::invalidate_imgui_texture() {
    default_images_.invalidate_imgui_texture();
}

void SsrPass::record(const FrameContext& ctx) {
    record_into(ctx, default_images_);
}

void SsrPass::record_into(const FrameContext& ctx, Images& imgs) {
    auto target_extent = ctx.target->extent();
    uint32_t width = target_extent.width;
    uint32_t height = target_extent.height;

    if (imgs.ssr_image == VK_NULL_HANDLE) return;
    if (ctx.prev_frame_color_view == VK_NULL_HANDLE) return;
    if (ctx.hiz_view == VK_NULL_HANDLE || ctx.hiz_mip_count == 0) return;

    auto cmd = ctx.cmd;
    auto global_set = ctx.global_descriptor_set;

    if (!enabled_) {
        insert_image_barrier(cmd, imgs.ssr_image,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        VkClearColorValue clear{};
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, imgs.ssr_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);

        insert_image_barrier(cmd, imgs.ssr_image,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        return;
    }

    uint32_t groups_x = (width + TILE_SIZE - 1) / TILE_SIZE;
    uint32_t groups_y = (height + TILE_SIZE - 1) / TILE_SIZE;

    // Update per-frame descriptors (lazy)
    if (ctx.hiz_view != imgs.last_hiz_view) {
        imgs.last_hiz_view = ctx.hiz_view;

        VkDescriptorImageInfo info{};
        info.sampler = ctx.hiz_sampler;
        info.imageView = ctx.hiz_view;
        info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = imgs.trace_set;
        write.dstBinding = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &info;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }

    if (ctx.prev_frame_color_view != imgs.last_prev_color_view) {
        imgs.last_prev_color_view = ctx.prev_frame_color_view;

        VkDescriptorImageInfo info{};
        info.sampler = linear_sampler_;
        info.imageView = ctx.prev_frame_color_view;
        info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = imgs.trace_set;
        write.dstBinding = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &info;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }

    VkImageView color_view = ctx.target->color_view();
    if (color_view != imgs.last_color_view) {
        imgs.last_color_view = color_view;

        VkDescriptorImageInfo info{};
        info.imageView = color_view;
        info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = imgs.composite_set;
        write.dstBinding = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        write.descriptorCount = 1;
        write.pImageInfo = &info;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }

    if (ctx.mr_view != VK_NULL_HANDLE && ctx.mr_view != imgs.last_mr_view) {
        imgs.last_mr_view = ctx.mr_view;

        VkDescriptorImageInfo info{};
        info.sampler = nearest_sampler_;
        info.imageView = ctx.mr_view;
        info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = imgs.composite_set;
        write.dstBinding = 2;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &info;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }

    // SSR output → GENERAL for write
    insert_image_barrier(cmd, imgs.ssr_image,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

    // Pass 1: SSR Trace
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, trace_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, trace_pipeline_layout_,
                            0, 1, &global_set, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, trace_pipeline_layout_,
                            1, 1, &imgs.trace_set, 0, nullptr);

    SsrTracePushConstants trace_push{};
    trace_push.viewport_width = width;
    trace_push.viewport_height = height;
    trace_push.hiz_mip_count = ctx.hiz_mip_count;
    trace_push.max_steps = max_steps_;
    trace_push.max_distance = max_distance_;
    trace_push.thickness = thickness_;
    trace_push.intensity = intensity_;

    vkCmdPushConstants(cmd, trace_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(trace_push), &trace_push);
    vkCmdDispatch(cmd, groups_x, groups_y, 1);

    // Barrier: SSR write → composite read
    insert_image_barrier(cmd, imgs.ssr_image,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // Pass 2: SSR Composite
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, composite_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, composite_pipeline_layout_,
                            0, 1, &global_set, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, composite_pipeline_layout_,
                            1, 1, &imgs.composite_set, 0, nullptr);

    SsrCompositePushConstants comp_push{};
    comp_push.viewport_width = width;
    comp_push.viewport_height = height;
    comp_push.intensity = intensity_;

    vkCmdPushConstants(cmd, composite_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(comp_push), &comp_push);
    vkCmdDispatch(cmd, groups_x, groups_y, 1);
}

} // namespace fjell
