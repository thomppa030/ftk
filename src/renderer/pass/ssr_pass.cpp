#include "renderer/pass/ssr_pass.hpp"
#include "renderer/frame_descriptor_cache.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/gpu_context.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/post_process_resources.hpp"
#include "renderer/resource_registry.hpp"
#include "renderer/gpu/vma_image.hpp"
#include "renderer/resources/shader_utils.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#include <array>
#include <stdexcept>

namespace fjell {

static constexpr uint32_t TILE_SIZE = 8;

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
                                           const std::string& shader_path,
                                           VkPipelineCache pipeline_cache = VK_NULL_HANDLE) {
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
    if (vkCreateComputePipelines(device, pipeline_cache, 1, &ci, nullptr, &pipeline) != VK_SUCCESS) {
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

// ── SsrPass (singleton) ─────────────────────────────────────────────────────

Result<> SsrPass::create(Device& device, const PassResources& resources) {
    device_ = device.handle();
    vk_pipeline_cache_ = resources.pipeline_cache;
    allocator_ = resources.allocator;
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
    trace_pipeline_ = create_compute_pipeline(device_, trace_pipeline_layout_, "shaders/ssr_trace.comp.spv", vk_pipeline_cache_);
    composite_pipeline_ = create_compute_pipeline(device_, composite_pipeline_layout_, "shaders/ssr_composite.comp.spv", vk_pipeline_cache_);

    FJELL_GFX_DEBUG("SSR pass created");
    return {};
}

void SsrPass::destroy() {
    // Prev-frame history buffer
    if (prev_frame_view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, prev_frame_view_, nullptr);
        prev_frame_view_ = VK_NULL_HANDLE;
    }
    destroy_vma_image(allocator_, prev_frame_image_, prev_frame_allocation_);
    prev_frame_image_ = VK_NULL_HANDLE;
    prev_frame_allocation_ = VK_NULL_HANDLE;
    prev_frame_extent_ = {};

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

void SsrPass::ensure_prev_frame(VkDevice device, VmaAllocator allocator,
                                 VkCommandPool command_pool, VkQueue queue,
                                 uint32_t width, uint32_t height) {
    if (prev_frame_image_ != VK_NULL_HANDLE &&
        width == prev_frame_extent_.width &&
        height == prev_frame_extent_.height) {
        return;
    }

    vkDeviceWaitIdle(device);

    if (prev_frame_view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device, prev_frame_view_, nullptr);
        prev_frame_view_ = VK_NULL_HANDLE;
    }
    destroy_vma_image(allocator, prev_frame_image_, prev_frame_allocation_);
    prev_frame_image_ = VK_NULL_HANDLE;
    prev_frame_allocation_ = VK_NULL_HANDLE;

    prev_frame_extent_ = {width, height};

    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    ci.extent = {width, height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    create_vma_image(allocator, &ci, &prev_frame_image_, &prev_frame_allocation_);

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = prev_frame_image_;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(device, &vi, nullptr, &prev_frame_view_);

    // Clear to black and transition to SHADER_READ_ONLY so first-frame SSR
    // reads valid (black) data.
    VkCommandBuffer init_cmd;
    VkCommandBufferAllocateInfo cmd_ai{};
    cmd_ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_ai.commandPool = command_pool;
    cmd_ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_ai.commandBufferCount = 1;
    vkAllocateCommandBuffers(device, &cmd_ai, &init_cmd);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(init_cmd, &begin);

    transition_image(init_cmd, prev_frame_image_,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

    VkClearColorValue black{};
    VkImageSubresourceRange clear_range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(init_cmd, prev_frame_image_,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &clear_range);

    transition_image(init_cmd, prev_frame_image_,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    vkEndCommandBuffer(init_cmd);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &init_cmd;
    vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(device, command_pool, 1, &init_cmd);
}

void SsrPass::copy_prev_frame(VkCommandBuffer cmd, VkImage src_color, VkExtent2D extent) {
    if (!enabled_ || prev_frame_image_ == VK_NULL_HANDLE) return;

    // src color → TRANSFER_SRC
    transition_image(cmd, src_color,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);

    // prev_frame → TRANSFER_DST
    transition_image(cmd, prev_frame_image_,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {extent.width, extent.height, 1};
    vkCmdCopyImage(cmd,
        src_color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        prev_frame_image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &region);

    // prev_frame → SHADER_READ_ONLY for next frame's SSR trace
    transition_image(cmd, prev_frame_image_,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

void SsrPass::record(const FrameContext& ctx) {
    auto target_extent = ctx.target->extent();
    uint32_t width = target_extent.width;
    uint32_t height = target_extent.height;

    if (ctx.registry == nullptr) return;

    auto* hiz_res = ctx.registry->find_image("hiz");
    auto* prev_color_res = ctx.registry->find_image("prev_frame_color");
    auto mr_view = ctx.registry->image_view("mr");
    auto* ssr_res = ctx.registry->find_image("ssr_output");

    if (prev_color_res == nullptr || prev_color_res->view == VK_NULL_HANDLE) return;
    if (hiz_res == nullptr || hiz_res->view == VK_NULL_HANDLE || hiz_res->mip_count == 0) return;
    // The composite set binds MR — if it's not yet registered, SSR can't
    // dispatch this frame. Old path left the MR slot stale but the cache
    // requires every binding in the layout to have a valid view.
    if (mr_view == VK_NULL_HANDLE) return;
    if (ssr_res == nullptr || ssr_res->image == VK_NULL_HANDLE) return;

    const VkImage ssr_image = ssr_res->image;
    const VkImageView ssr_view = ssr_res->view;

    auto cmd = ctx.cmd;
    auto global_set = ctx.global_descriptor_set;

    if (!enabled_) {
        insert_image_barrier(cmd, ssr_image,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        VkClearColorValue clear{};
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, ssr_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);

        insert_image_barrier(cmd, ssr_image,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        return;
    }

    uint32_t groups_x = (width + TILE_SIZE - 1) / TILE_SIZE;
    uint32_t groups_y = (height + TILE_SIZE - 1) / TILE_SIZE;

    auto* cache = ctx.descriptor_cache;

    // Acquire trace set: hiz (0), prev_color (1), ssr output (2).
    // ssr output descriptor references GENERAL layout — Vulkan demands
    // the descriptor's imageLayout match the actual image state at use
    // time, and we barrier to GENERAL right before dispatch below.
    std::array<FrameCacheBinding, 3> trace_bindings{};
    trace_bindings[0].binding = 0;
    trace_bindings[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    trace_bindings[0].image.sampler = hiz_res->sampler;
    trace_bindings[0].image.imageView = hiz_res->view;
    trace_bindings[0].image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    trace_bindings[1].binding = 1;
    trace_bindings[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    trace_bindings[1].image.sampler = linear_sampler_;
    trace_bindings[1].image.imageView = prev_color_res->view;
    trace_bindings[1].image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    trace_bindings[2].binding = 2;
    trace_bindings[2].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    trace_bindings[2].image.imageView = ssr_view;
    trace_bindings[2].image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorSet trace_set = cache->acquire(trace_set_layout_, trace_bindings);

    // Acquire composite set: ssr (0), color output (1), mr (2).
    std::array<FrameCacheBinding, 3> comp_bindings{};
    comp_bindings[0].binding = 0;
    comp_bindings[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    comp_bindings[0].image.sampler = nearest_sampler_;
    comp_bindings[0].image.imageView = ssr_view;
    comp_bindings[0].image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    comp_bindings[1].binding = 1;
    comp_bindings[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    comp_bindings[1].image.imageView = ctx.target->color_view();
    comp_bindings[1].image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    comp_bindings[2].binding = 2;
    comp_bindings[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    comp_bindings[2].image.sampler = nearest_sampler_;
    comp_bindings[2].image.imageView = mr_view;
    comp_bindings[2].image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorSet composite_set = cache->acquire(composite_set_layout_, comp_bindings);

    // SSR output → GENERAL for write
    insert_image_barrier(cmd, ssr_image,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

    // Pass 1: SSR Trace
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, trace_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, trace_pipeline_layout_,
                            0, 1, &global_set, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, trace_pipeline_layout_,
                            1, 1, &trace_set, 0, nullptr);

    SsrTracePushConstants trace_push{};
    trace_push.viewport_width = width;
    trace_push.viewport_height = height;
    trace_push.hiz_mip_count = hiz_res->mip_count;
    trace_push.max_steps = max_steps_;
    trace_push.max_distance = max_distance_;
    trace_push.thickness = thickness_;
    trace_push.intensity = intensity_;

    vkCmdPushConstants(cmd, trace_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(trace_push), &trace_push);
    vkCmdDispatch(cmd, groups_x, groups_y, 1);

    // Barrier: SSR write → composite read
    insert_image_barrier(cmd, ssr_image,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // Pass 2: SSR Composite
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, composite_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, composite_pipeline_layout_,
                            0, 1, &global_set, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, composite_pipeline_layout_,
                            1, 1, &composite_set, 0, nullptr);

    SsrCompositePushConstants comp_push{};
    comp_push.viewport_width = width;
    comp_push.viewport_height = height;
    comp_push.intensity = intensity_;

    vkCmdPushConstants(cmd, composite_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(comp_push), &comp_push);
    vkCmdDispatch(cmd, groups_x, groups_y, 1);
}

void SsrPass::pre_record(const FrameContext& ctx, GpuContext& gpu) {
    bool needs_buffer = ctx.pp_resources && ctx.pp_resources->config().ssr;
    if (needs_buffer && ctx.target) {
        auto ext = ctx.target->extent();
        ensure_prev_frame(gpu.device(), gpu.allocator(),
                          gpu.command_pool(), gpu.graphics_queue(),
                          ext.width, ext.height);
    }
}

void SsrPass::post_record(const FrameContext& ctx) {
    bool needs_copy = ctx.pp_resources && ctx.pp_resources->config().ssr;
    if (needs_copy && ctx.target) {
        copy_prev_frame(ctx.cmd, ctx.target->color_image(), ctx.target->extent());
    }
}

void SsrPass::register_resources(ResourceRegistry& registry) {
    registry.register_image("prev_frame_color",
        {.frame_graph_id = 0, .image = prev_frame_image_, .view = prev_frame_view_});
}

PassSetup SsrPass::setup() const {
    return {
        .name = "ssr",
        .phase = RenderPhase::PostProcess,
        .sub_order = 0,
    };
}

void SsrPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    auto depth = builder.import_named(ctx, "depth");
    auto color = builder.import_named(ctx, "color");
    auto mr = builder.import_named(ctx, "mr");
    builder.read(depth, ResourceAccess::sampled_compute);
    builder.write(color, ResourceAccess::storage_write_compute);
    builder.read(mr, ResourceAccess::sampled_compute);

    // SSR intermediate (full-res R16G16B16A16F) is a per-frame transient;
    // declare it so Phase 3 aliasing can pack it with similar-shape
    // resources. The prev_frame_color history stays pass-private —
    // it's persistent across frames.
    TextureDesc ssr_desc{};
    ssr_desc.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    ssr_desc.size_class = SizeClass::viewport;
    auto ssr_out = builder.create("ssr_output", ssr_desc);
    builder.write(ssr_out, ResourceAccess::storage_write_compute);
    builder.read(ssr_out, ResourceAccess::sampled_compute);
    // SSR-off path clears the output via vkCmdClearColorImage — the
    // allocation needs TRANSFER_DST to support that operation.
    builder.write(ssr_out, ResourceAccess::transfer_dst);
}

} // namespace fjell
