#include "renderer/pass/contact_shadow_pass.hpp"
#include "core/profiler.hpp"
#include "renderer/pass/pass_gpu_zone.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/gpu/vma_image.hpp"
#include "renderer/gpu_context.hpp"
#include "renderer/global_bindings.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/pass_registry.hpp"
#include "renderer/renderer_constants.hpp"
#include "renderer/resource_registry.hpp"
#include "renderer/gpu/shader_utils.hpp"
#include "environment/environment_state.hpp"
#include "scene/scene.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace fjell {

struct ContactShadowPush {
    glm::uvec2 viewport_size;
    float max_distance;
    float thickness;
    uint32_t step_count;
    float frame_jitter;
    float pad0;
    float pad1;
};

namespace {

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
        throw std::runtime_error("Failed to create contact shadow compute pipeline");
    }
    vkDestroyShaderModule(device, module, nullptr);
    return pipeline;
}

void batch_barriers(VkCommandBuffer cmd,
                    const VkImageMemoryBarrier2* barriers, uint32_t count) {
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

Result<> ContactShadowPass::create(Device& device, const PassResources& resources) {
    device_ = device.handle();
    allocator_ = resources.allocator;
    vk_pipeline_cache_ = resources.pipeline_cache;
    auto global_set_layout = resources.global_set_layout;

    // Linear sampler for depth_texture reads. (Depth sampled as float in a
    // compute shader uses a regular 2D sampler, not the shadow comparison.)
    VkSamplerCreateInfo sampler_ci{};
    sampler_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_ci.magFilter = VK_FILTER_LINEAR;
    sampler_ci.minFilter = VK_FILTER_LINEAR;
    sampler_ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device_, &sampler_ci, nullptr, &sampler_) != VK_SUCCESS) {
        return make_error("Failed to create contact shadow sampler");
    }

    // Set 1 binding 0: output R8 storage image.
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
        vkCreateDescriptorSetLayout(device_, &ci, nullptr, &set_layout_);
    }

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(ContactShadowPush);

    std::array layouts{global_set_layout, set_layout_};
    VkPipelineLayoutCreateInfo pl_ci{};
    pl_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl_ci.setLayoutCount = static_cast<uint32_t>(layouts.size());
    pl_ci.pSetLayouts = layouts.data();
    pl_ci.pushConstantRangeCount = 1;
    pl_ci.pPushConstantRanges = &push_range;
    if (vkCreatePipelineLayout(device_, &pl_ci, nullptr, &pipeline_layout_) != VK_SUCCESS) {
        return make_error("Failed to create contact shadow pipeline layout");
    }

    pipeline_ = create_compute_pipeline(device_, pipeline_layout_,
                                        "shaders/contact_shadow.comp.spv",
                                        vk_pipeline_cache_);

    // Descriptor pool for a single storage-image set.
    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1};
    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.maxSets = 1;
    pool_ci.poolSizeCount = 1;
    pool_ci.pPoolSizes = &pool_size;
    vkCreateDescriptorPool(device_, &pool_ci, nullptr, &pool_);

    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = pool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &set_layout_;
    vkAllocateDescriptorSets(device_, &alloc, &output_set_);

    // Start at a common desktop size and grow from ensure_size() as viewports
    // demand it. The compute shader writes only the region named by
    // viewport_size in its push constants, so an image larger than the
    // viewport is fine; one smaller is not, because the stores outside it are
    // discarded and shading then reads texels that were never written.
    // Growing happens between frames, never during record(), which is what
    // made the earlier resize-on-record approach a descriptor-invalidation
    // storm.
    create_image(1920, 1080);

    FJELL_GFX_DEBUG("Contact shadow pass created");
    return {};
}

void ContactShadowPass::post_create(GpuContext& gpu, PassRegistry& /*passes*/) {
    // Remember these so a later regrow can seed its image the same way.
    graphics_queue_ = gpu.core().graphics_queue();
    command_pool_ = gpu.core().command_pool();

    // Transition the image to SHADER_READ_ONLY_OPTIMAL and seed all frames'
    // global binding with its view so the descriptor is valid from the very
    // first draw.
    seed_layout();

    auto& gb = gpu.global_bindings();
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        gb.write_image(i, "contact_shadow",
            shadow_view_, sampler_,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    gb.flush_writes();
}

void ContactShadowPass::destroy() {
    if (device_ == VK_NULL_HANDLE) return;

    destroy_image();

    if (pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, pool_, nullptr);
        pool_ = VK_NULL_HANDLE;
        output_set_ = VK_NULL_HANDLE;
    }
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (pipeline_layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
        pipeline_layout_ = VK_NULL_HANDLE;
    }
    if (set_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
        set_layout_ = VK_NULL_HANDLE;
    }
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, sampler_, nullptr);
        sampler_ = VK_NULL_HANDLE;
    }
    device_ = VK_NULL_HANDLE;
}

void ContactShadowPass::create_image(uint32_t w, uint32_t h) {
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = SHADOW_FORMAT;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (create_vma_image(allocator_, &ci, &shadow_image_, &shadow_memory_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create contact shadow image");
    }
    set_image_debug_name(device_, shadow_image_, "contact_shadow");

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = shadow_image_;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = SHADOW_FORMAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device_, &vi, nullptr, &shadow_view_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create contact shadow view");
    }

    extent_ = {w, h};

    update_descriptor();
}

void ContactShadowPass::destroy_image() {
    if (shadow_view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, shadow_view_, nullptr);
        shadow_view_ = VK_NULL_HANDLE;
    }
    destroy_vma_image(allocator_, shadow_image_, shadow_memory_);
    shadow_image_ = VK_NULL_HANDLE;
    shadow_memory_ = VK_NULL_HANDLE;
    extent_ = {};
}

void ContactShadowPass::update_descriptor() {
    VkDescriptorImageInfo info{};
    info.imageView = shadow_view_;
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

void ContactShadowPass::seed_layout() {
    if (graphics_queue_ == VK_NULL_HANDLE || shadow_image_ == VK_NULL_HANDLE) return;

    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = command_pool_;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd{};
    vkAllocateCommandBuffers(device_, &alloc, &cmd);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    auto barrier = make_barrier(shadow_image_,
        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    batch_barriers(cmd, &barrier, 1);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(graphics_queue_, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphics_queue_);
    vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
}

void ContactShadowPass::ensure_size(uint32_t w, uint32_t h) {
    if (w <= extent_.width && h <= extent_.height) return;
    // One image serves every viewport, so grow to cover the largest rather
    // than thrashing between sizes as different panels ask for different ones.
    resize(std::max(w, extent_.width), std::max(h, extent_.height));
}

void ContactShadowPass::resize(uint32_t w, uint32_t h) {
    if (w == extent_.width && h == extent_.height) return;
    vkDeviceWaitIdle(device_);
    destroy_image();
    create_image(w, h);
    // Seed the replacement immediately rather than waiting for this pass to
    // record. The global descriptor already points at the new view, and the
    // draws that sample it live in a command buffer this pass does not
    // record into, so they can reach it first and read an UNDEFINED image.
    seed_layout();
}

void ContactShadowPass::prepare_frame(const FrameContext& ctx, GpuContext& /*gpu*/) {
    if (ctx.environment == nullptr) return;
    const auto& cs = ctx.environment->contact_shadows;
    enabled_ = cs.enabled;
    max_distance_ = cs.distance;
    thickness_ = cs.thickness;
    step_count_ = cs.steps;
}

PassSetup ContactShadowPass::setup() const {
    return {
        .name = "contact_shadow",
        // Traces the prepass depth, so it runs once that is resolved and
        // before the colour passes whose surfaces sample the result.
        .phase = RenderPhase::GpuDriven,
        .sub_order = 105,   // after GTAO (100-102)
    };
}

void ContactShadowPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    // The graph moves the image into the layout the write needs; record()
    // moves it back to where the surfaces sample it, which final_layout
    // reports.
    auto shadow = builder.import_named(ctx, "contact_shadow");
    if (!enabled_) {
        builder.write(shadow, ResourceAccess::transfer_dst);
        builder.final_layout(shadow, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                             VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        return;
    }
    builder.read(builder.import_named(ctx, "depth"), ResourceAccess::sampled_compute);
    builder.write(shadow, ResourceAccess::storage_write_compute);
    builder.final_layout(shadow, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

void ContactShadowPass::collect_exports(DeclareContext& ctx) {
    if (shadow_image_ == VK_NULL_HANDLE) { return; }
    ImportedImage img{};
    img.image = shadow_image_;
    img.view = shadow_view_;
    // Seeded when created or regrown, and every record leaves it there.
    img.initial_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ctx.imports.emplace(fg_name_hash("contact_shadow"), img);
}

void ContactShadowPass::record(const FrameContext& ctx) {
    if (ctx.target == nullptr) return;

    FJELL_PROFILE_SCOPE_N("record_contact_shadow");
    FJELL_GPU_ZONE(ctx, "Contact Shadow");

    auto extent = ctx.target->extent();
    VkCommandBuffer cmd = ctx.cmd;

    // Disabled: hold 1.0, no occlusion, for the surfaces that still sample
    // it. The graph has the image in TRANSFER_DST_OPTIMAL.
    if (!enabled_) {
        VkClearColorValue clear{};
        clear.float32[0] = 1.0f;
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, shadow_image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &clear, 1, &range);

        auto to_sampled = make_barrier(shadow_image_,
            VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        batch_barriers(cmd, &to_sampled, 1);

        ++frame_counter_;
        return;
    }

    // The graph has the image in GENERAL for the storage write.
    ContactShadowPush push{};
    push.viewport_size = glm::uvec2(extent.width, extent.height);
    push.max_distance = max_distance_;
    push.thickness = thickness_;
    push.step_count = static_cast<uint32_t>(std::max(step_count_, 4));
    push.frame_jitter = static_cast<float>(frame_counter_ % 64) * (1.0f / 64.0f);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout_,
                            0, 1, &ctx.global_descriptor_set, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout_,
                            1, 1, &output_set_, 0, nullptr);
    vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);

    uint32_t groups_x = (extent.width + TILE_SIZE - 1) / TILE_SIZE;
    uint32_t groups_y = (extent.height + TILE_SIZE - 1) / TILE_SIZE;
    vkCmdDispatch(cmd, groups_x, groups_y, 1);

    auto to_sampled = make_barrier(shadow_image_,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    batch_barriers(cmd, &to_sampled, 1);

    ++frame_counter_;
}

} // namespace fjell
