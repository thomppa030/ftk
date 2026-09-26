#include "renderer/pass/cloud_shadow_pass.hpp"

#include "core/log.hpp"
#include "core/profiler.hpp"
#include "environment/environment_state.hpp"
#include "renderer/global_bindings.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/gpu/vk_check.hpp"
#include "renderer/gpu/vma_image.hpp"
#include "renderer/gpu_context.hpp"
#include "renderer/pass/pass_gpu_zone.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/pass_registry.hpp"
#include "renderer/renderer_constants.hpp"
#include "renderer/gpu/shader_utils.hpp"
#include "core/base_path.hpp"

#include <array>
#include <stdexcept>
#include <string>

namespace fjell {

namespace {

struct CloudShadowPush {
    glm::vec2 min_xz;
    float size;
    float texture_size;
    float ground_y;
    float time;
    glm::vec2 pad;
};

VkPipeline create_compute_pipeline(VkDevice device, VkPipelineLayout layout,
                                   const std::string& shader_path,
                                   VkPipelineCache pipeline_cache) {
    auto code = read_spirv(base_path::resolve(shader_path));
    auto* module = create_shader_module(device, code);

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
        throw std::runtime_error("Failed to create cloud shadow compute pipeline");
    }
    vkDestroyShaderModule(device, module, nullptr);
    return pipeline;
}

void batch_barriers(VkCommandBuffer cmd, const VkImageMemoryBarrier2* barriers, uint32_t count) {
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = count;
    dep.pImageMemoryBarriers = barriers;
    vkCmdPipelineBarrier2(cmd, &dep);
}

VkImageMemoryBarrier2 make_barrier(VkImage image,
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

} // namespace

Result<> CloudShadowPass::create(Device& device, const PassResources& resources) {
    device_ = device.handle();
    allocator_ = resources.allocator;
    vk_pipeline_cache_ = resources.pipeline_cache;

    // Linear sampling across the map: a texel is two metres of ground and
    // the shadow edge should not show it.
    VkSamplerCreateInfo sampler_ci{};
    sampler_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_ci.magFilter = VK_FILTER_LINEAR;
    sampler_ci.minFilter = VK_FILTER_LINEAR;
    sampler_ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device_, &sampler_ci, nullptr, &sampler_) != VK_SUCCESS) {
        return make_error("Failed to create cloud shadow sampler");
    }

    // Set 1 binding 0: the map as a storage image.
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
        vk_check(vkCreateDescriptorSetLayout(device_, &ci, nullptr, &set_layout_),
                 "Failed to create cloud shadow descriptor set layout");
    }

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(CloudShadowPush);

    std::array layouts{resources.global_set_layout, set_layout_};
    VkPipelineLayoutCreateInfo pl_ci{};
    pl_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl_ci.setLayoutCount = static_cast<uint32_t>(layouts.size());
    pl_ci.pSetLayouts = layouts.data();
    pl_ci.pushConstantRangeCount = 1;
    pl_ci.pPushConstantRanges = &push_range;
    if (vkCreatePipelineLayout(device_, &pl_ci, nullptr, &pipeline_layout_) != VK_SUCCESS) {
        return make_error("Failed to create cloud shadow pipeline layout");
    }

    pipeline_ = create_compute_pipeline(device_, pipeline_layout_,
                                        "shaders/cloud_shadow.comp.spv", vk_pipeline_cache_);

    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1};
    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.maxSets = 1;
    pool_ci.poolSizeCount = 1;
    pool_ci.pPoolSizes = &pool_size;
    vk_check(vkCreateDescriptorPool(device_, &pool_ci, nullptr, &pool_),
             "Failed to create cloud shadow descriptor pool");

    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = pool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &set_layout_;
    vk_check(vkAllocateDescriptorSets(device_, &alloc, &output_set_),
             "Failed to allocate cloud shadow descriptor set");

    create_image();
    FJELL_GFX_DEBUG("Cloud shadow pass created");
    return {};
}

void CloudShadowPass::post_create(GpuContext& gpu, PassRegistry& /*passes*/) {
    graphics_queue_ = gpu.core().graphics_queue();
    command_pool_ = gpu.core().command_pool();

    // Clear to full transmittance and make the image readable before any
    // draw samples it: graphs that never run this pass still bind it.
    seed_layout();

    auto& gb = gpu.global_bindings();
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        gb.write_image(i, "cloud_shadow", view_, sampler_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    gb.flush_writes();
}

void CloudShadowPass::destroy() {
    if (device_ == VK_NULL_HANDLE) return;
    destroy_image();
    if (pool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(device_, pool_, nullptr);
    if (pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device_, pipeline_, nullptr);
    if (pipeline_layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    if (set_layout_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
    if (sampler_ != VK_NULL_HANDLE) vkDestroySampler(device_, sampler_, nullptr);
    pool_ = VK_NULL_HANDLE;
    output_set_ = VK_NULL_HANDLE;
    pipeline_ = VK_NULL_HANDLE;
    pipeline_layout_ = VK_NULL_HANDLE;
    set_layout_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
}

void CloudShadowPass::create_image() {
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = SHADOW_FORMAT;
    ci.extent = {RESOLUTION, RESOLUTION, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (create_vma_image(allocator_, &ci, &image_, &memory_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create cloud shadow image");
    }
    set_image_debug_name(device_, image_, "cloud_shadow");

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = image_;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = SHADOW_FORMAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device_, &vi, nullptr, &view_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create cloud shadow view");
    }

    VkDescriptorImageInfo info{};
    info.imageView = view_;
    info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = output_set_;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.descriptorCount = 1;
    write.pImageInfo = &info;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
}

void CloudShadowPass::destroy_image() {
    if (view_ != VK_NULL_HANDLE) vkDestroyImageView(device_, view_, nullptr);
    view_ = VK_NULL_HANDLE;
    destroy_vma_image(allocator_, image_, memory_);
    image_ = VK_NULL_HANDLE;
    memory_ = VK_NULL_HANDLE;
}

void CloudShadowPass::seed_layout() {
    if (graphics_queue_ == VK_NULL_HANDLE || image_ == VK_NULL_HANDLE) return;
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = command_pool_;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd{};
    vk_check(vkAllocateCommandBuffers(device_, &alloc, &cmd), "Failed to allocate cloud shadow seed command buffer");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(cmd, &begin), "Failed to begin cloud shadow seed command buffer");

    auto to_transfer = make_barrier(image_,
        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
        VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    batch_barriers(cmd, &to_transfer, 1);
    VkClearColorValue clear{};
    clear.float32[0] = 1.0f;
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    auto to_sampled = make_barrier(image_,
        VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    batch_barriers(cmd, &to_sampled, 1);

    vk_check(vkEndCommandBuffer(cmd), "Failed to end cloud shadow seed command buffer");
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vk_check(vkQueueSubmit(graphics_queue_, 1, &submit, VK_NULL_HANDLE), "Failed to submit cloud shadow seed");
    vk_check(vkQueueWaitIdle(graphics_queue_), "Failed to wait for cloud shadow seed");
    vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
    cleared_ = true;
}

void CloudShadowPass::prepare_frame(const FrameContext& ctx, GpuContext& /*gpu*/) {
    active_ = false;
    if (ctx.environment == nullptr) return;
    const auto& env = *ctx.environment;
    // Above the grazing band the compute shader fades the shadow out on its
    // own; below it there is nothing to draw, so the map holds full
    // transmittance and the dispatch is skipped entirely.
    active_ = env.weather.cloud_coverage > 0.0f && env.primary.lit() &&
              env.primary.direction.y > 0.05f;
    frame_ = cloud_shadow_frame(ctx.camera_position, EXTENT_METRES, RESOLUTION);
    // The ground the sun's rays are traced from: the camera's height is the
    // one height the map knows about the terrain under it.
    ground_y_ = ctx.camera_position.y - 2.0f;
    time_ += ctx.delta_time;
}

PassSetup CloudShadowPass::setup() const {
    return {
        .name = "cloud_shadow",
        .phase = RenderPhase::Environment,
        .sub_order = 60,   // after the sky cubemap, before anything shades
    };
}

void CloudShadowPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    // Nothing to do while the map already holds full transmittance. The
    // graph moves the image into the layout the clear or the dispatch needs;
    // record() moves it back to where the surfaces, the fog light and the
    // sun shafts sample it, which final_layout reports.
    if (!active_ && cleared_) { return; }
    auto map = builder.import_named(ctx, "cloud_shadow");
    constexpr VkPipelineStageFlags2 readers =
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    if (!active_) {
        builder.write(map, ResourceAccess::transfer_dst);
        builder.final_layout(map, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                             readers, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        return;
    }
    builder.write(map, ResourceAccess::storage_write_compute);
    builder.final_layout(map, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                         readers, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

void CloudShadowPass::collect_exports(DeclareContext& ctx) {
    if (image_ == VK_NULL_HANDLE) { return; }
    ImportedImage img{};
    img.image = image_;
    img.view = view_;
    // Seeded when created, and every record leaves it there.
    img.initial_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ctx.imports.emplace(fg_name_hash("cloud_shadow"), img);
}

void CloudShadowPass::record(const FrameContext& ctx) {
    FJELL_PROFILE_SCOPE_N("record_cloud_shadow");
    VkCommandBuffer cmd = ctx.cmd;

    if (!active_) {
        // No clouds or no sun: hold full transmittance. Once cleared the map
        // stays cleared until clouds return, so this costs nothing per frame.
        // The graph has the image in TRANSFER_DST_OPTIMAL.
        if (cleared_) return;
        VkClearColorValue clear{};
        clear.float32[0] = 1.0f;
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
        auto to_sampled = make_barrier(image_,
            VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        batch_barriers(cmd, &to_sampled, 1);
        cleared_ = true;
        return;
    }

    // The graph has the image in GENERAL for the storage write.
    FJELL_GPU_ZONE(ctx, "Cloud Shadow");
    CloudShadowPush push{};
    push.min_xz = frame_.min_xz;
    push.size = frame_.size;
    push.texture_size = static_cast<float>(RESOLUTION);
    push.ground_y = ground_y_;
    push.time = time_;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    std::array sets{ctx.global_descriptor_set, output_set_};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout_, 0,
                            static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
    vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    const uint32_t groups = (RESOLUTION + 7) / 8;
    vkCmdDispatch(cmd, groups, groups, 1);

    auto to_sampled = make_barrier(image_,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    batch_barriers(cmd, &to_sampled, 1);
    cleared_ = false;
}

} // namespace fjell
