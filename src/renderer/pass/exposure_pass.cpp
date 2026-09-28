#include "renderer/pass/exposure_pass.hpp"
#include "core/profiler.hpp"
#include "renderer/pass/pass_gpu_zone.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/resource_registry.hpp"
#include "renderer/gpu/shader_utils.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#include <cstring>

namespace fjell {

struct ExposurePushConstants {
    uint32_t width;
    uint32_t height;
    float min_log_lum;
    float inv_log_range;
    uint32_t pass_id;
};

Result<> ExposurePass::create(Device& device, const PassResources& resources) {
    device_ = device.handle();
    allocator_ = resources.allocator;

    // Sampler for HDR texture
    VkSamplerCreateInfo samp_ci{};
    samp_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samp_ci.magFilter = VK_FILTER_LINEAR;
    samp_ci.minFilter = VK_FILTER_LINEAR;
    samp_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samp_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(device_, &samp_ci, nullptr, &sampler_);

    // Descriptor set layout: binding 0 = HDR sampler, binding 1 = histogram SSBO
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_ci.bindingCount = 2;
    layout_ci.pBindings = bindings;
    vkCreateDescriptorSetLayout(device_, &layout_ci, nullptr, &set_layout_);

    // Push constants
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(ExposurePushConstants);

    VkPipelineLayoutCreateInfo pipe_layout_ci{};
    pipe_layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipe_layout_ci.setLayoutCount = 1;
    pipe_layout_ci.pSetLayouts = &set_layout_;
    pipe_layout_ci.pushConstantRangeCount = 1;
    pipe_layout_ci.pPushConstantRanges = &push_range;
    if (vkCreatePipelineLayout(device_, &pipe_layout_ci, nullptr, &layout_) != VK_SUCCESS) {
        return make_error("Failed to create exposure pipeline layout");
    }

    // Compute pipeline
    auto comp_code = read_spirv(base_path::resolve("shaders/exposure_histogram.comp.spv"));
    auto comp_module = create_shader_module(device_, comp_code);

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = comp_module;
    stage.pName = "main";

    VkComputePipelineCreateInfo pipeline_ci{};
    pipeline_ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_ci.stage = stage;
    pipeline_ci.layout = layout_;

    if (vkCreateComputePipelines(device_, resources.pipeline_cache, 1, &pipeline_ci,
                                  nullptr, &pipeline_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, comp_module, nullptr);
        return make_error("Failed to create exposure compute pipeline");
    }
    vkDestroyShaderModule(device_, comp_module, nullptr);

    // Per-frame histogram buffers (256 uint bins + 1 float avg)
    constexpr VkDeviceSize buf_size = 256 * sizeof(uint32_t) + sizeof(float);
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        buffers_[i] = std::make_unique<Buffer>(
            allocator_, buf_size,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VMA_MEMORY_USAGE_GPU_ONLY);
        buffers_[i]->share(*resources.device);
    }

    // Descriptor pool + per-frame sets
    VkDescriptorPoolSize pool_sizes[2]{};
    pool_sizes[0] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_FRAMES_IN_FLIGHT};
    pool_sizes[1] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, MAX_FRAMES_IN_FLIGHT};

    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.maxSets = MAX_FRAMES_IN_FLIGHT;
    pool_ci.poolSizeCount = 2;
    pool_ci.pPoolSizes = pool_sizes;
    vkCreateDescriptorPool(device_, &pool_ci, nullptr, &pool_);

    std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> layouts;
    layouts.fill(set_layout_);
    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = pool_;
    alloc_info.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    alloc_info.pSetLayouts = layouts.data();
    vkAllocateDescriptorSets(device_, &alloc_info, sets_.data());

    // Small host-visible readback buffers (1 float per frame)
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        readback_buffers_[i] = std::make_unique<Buffer>(
            allocator_, sizeof(float),
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT);
        readback_buffers_[i]->map();
        readback_buffers_[i]->share(*resources.device);
        float init = 0.18f;
        std::memcpy(readback_buffers_[i]->mapped(), &init, sizeof(float));
    }

    // Write SSBO descriptors (stable handles, don't change per frame)
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        VkDescriptorBufferInfo buf_info{};
        buf_info.buffer = buffers_[i]->handle();
        buf_info.offset = 0;
        buf_info.range = buf_size;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = sets_[i];
        write.dstBinding = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.descriptorCount = 1;
        write.pBufferInfo = &buf_info;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }

    FJELL_GFX_DEBUG("Exposure histogram pass created");
    return {};
}

void ExposurePass::destroy() {
    for (auto& buf : readback_buffers_) buf.reset();
    for (auto& buf : buffers_) buf.reset();
    if (pipeline_ != VK_NULL_HANDLE) { vkDestroyPipeline(device_, pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE; }
    if (layout_ != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device_, layout_, nullptr); layout_ = VK_NULL_HANDLE; }
    if (pool_ != VK_NULL_HANDLE) { vkDestroyDescriptorPool(device_, pool_, nullptr); pool_ = VK_NULL_HANDLE; }
    if (set_layout_ != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr); set_layout_ = VK_NULL_HANDLE; }
    if (sampler_ != VK_NULL_HANDLE) { vkDestroySampler(device_, sampler_, nullptr); sampler_ = VK_NULL_HANDLE; }
}

void ExposurePass::record(const FrameContext& ctx) {
    if (!enabled_) return;

    auto* target = ctx.target;
    if (!target) return;

    FJELL_PROFILE_SCOPE_N("record_exposure");
    FJELL_GPU_ZONE(ctx, "Exposure Histogram");

    auto extent = target->extent();
    auto cmd = ctx.cmd;

    // Update HDR texture descriptor (binding 0) for this frame
    VkDescriptorImageInfo img_info{};
    img_info.sampler = sampler_;
    img_info.imageView = target->color_view();
    img_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = sets_[ctx.frame_index];
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &img_info;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_,
                            0, 1, &sets_[ctx.frame_index], 0, nullptr);

    float log_range = max_log_lum_ - min_log_lum_;

    // Pass 1: Build histogram
    {
        ExposurePushConstants push{};
        push.width = extent.width;
        push.height = extent.height;
        push.min_log_lum = min_log_lum_;
        push.inv_log_range = 1.0f / log_range;
        push.pass_id = 0;

        vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);

        uint32_t groups_x = (extent.width + 15) / 16;
        uint32_t groups_y = (extent.height + 15) / 16;
        vkCmdDispatch(cmd, groups_x, groups_y, 1);
    }

    // Barrier: histogram writes complete before reduce reads
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

    // Pass 2: Reduce histogram to average luminance
    {
        ExposurePushConstants push{};
        push.width = extent.width;
        push.height = extent.height;
        push.min_log_lum = min_log_lum_;
        push.inv_log_range = 1.0f / log_range;
        push.pass_id = 1;

        vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);

        vkCmdDispatch(cmd, 1, 1, 1);
    }

    // Copy avg_luminance float from device-local SSBO to host-visible readback
    {
        VkMemoryBarrier2 copy_barrier{};
        copy_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        copy_barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        copy_barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        copy_barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        copy_barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;

        VkDependencyInfo copy_dep{};
        copy_dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        copy_dep.memoryBarrierCount = 1;
        copy_dep.pMemoryBarriers = &copy_barrier;
        vkCmdPipelineBarrier2(cmd, &copy_dep);

        VkBufferCopy region{};
        region.srcOffset = 256 * sizeof(uint32_t);  // avg_luminance offset
        region.dstOffset = 0;
        region.size = sizeof(float);
        vkCmdCopyBuffer(cmd, buffers_[ctx.frame_index]->handle(),
                        readback_buffers_[ctx.frame_index]->handle(), 1, &region);

        // The reader is the CPU, once this slot's fence has been waited on;
        // the fence alone does not make a device write visible to the host.
        VkBufferMemoryBarrier2 host_barrier{};
        host_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        host_barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        host_barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        host_barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        host_barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        host_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host_barrier.buffer = readback_buffers_[ctx.frame_index]->handle();
        host_barrier.offset = 0;
        host_barrier.size = VK_WHOLE_SIZE;

        VkDependencyInfo host_dep{};
        host_dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        host_dep.bufferMemoryBarrierCount = 1;
        host_dep.pBufferMemoryBarriers = &host_barrier;
        vkCmdPipelineBarrier2(cmd, &host_dep);
    }
}

float ExposurePass::average_luminance(uint32_t frame_index) const {
    if (!readback_buffers_[frame_index]) return 0.18f;
    float avg = 0.18f;
    std::memcpy(&avg, readback_buffers_[frame_index]->mapped(), sizeof(float));
    return avg;
}

PassSetup ExposurePass::setup() const {
    return {
        .name = "exposure",
        .phase = RenderPhase::PostProcess,
        .sub_order = 25,  // after bloom (0), before the post pass (300)
    };
}

void ExposurePass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    // Off without auto-exposure: nothing declared, and the graph culls it.
    if (!enabled_) { return; }
    auto color = builder.import_named(ctx, "color");
    builder.read(color, gpu::Access::sampled_compute);

    // The histogram is counted into and reduced in place, then copied from.
    // Every viewport in the frame uses this slot's, so declaring it orders
    // one viewport's count after the last one's copy.
    const uint32_t slot = ctx.frame_index;
    if (buffers_[slot] && readback_buffers_[slot]) {
        auto histogram = builder.import("exposure_histogram", buffers_[slot]->shared());
        builder.read_write(histogram, gpu::Access::storage_buffer_read_write_compute);
        builder.read(histogram, gpu::Access::copy_src);
        auto readback = builder.import("exposure_readback", readback_buffers_[slot]->shared());
        builder.write(readback, gpu::Access::copy_dst);
    }
    // The average's only reader is the CPU, on a later frame, which the
    // graph cannot see.
    builder.has_side_effects();
}

} // namespace fjell
