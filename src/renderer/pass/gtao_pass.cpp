// ── Screen-space AO pipeline implementation ─────────────────────────────────
//
// Three RenderPass classes, one compute dispatch each. The FrameGraph emits
// one merged barrier per image per node (RDG-style "merged access state"),
// so nodes declare only the access they need and avoid inline barriers. The
// single exception is AoBlurVPass's final ao_image GENERAL → SHADER_READ
// transition: it matches AoPass::declare's final_layout() promise and
// satisfies the baked SHADER_READ layout in the global descriptor
// (binding 5 "ao_texture") read by every subsequent frag shader.

#include "renderer/pass/gtao_pass.hpp"
#include "renderer/pass/pass_gpu_zone.hpp"
#include "renderer/frame_descriptor_cache.hpp"
#include "renderer/gpu/vma_image.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/gpu_context.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/pass_registry.hpp"
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

namespace {

constexpr uint32_t TILE_SIZE = 8;
constexpr VkFormat AO_FORMAT = VK_FORMAT_R8_UNORM;

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

void create_ao_image(VkDevice device, VmaAllocator allocator,
                      const uint32_t* concurrent_families, uint32_t concurrent_family_count,
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
    // GTAO writes ao_image on the compute queue; mesh.frag reads it
    // through the global descriptor on the graphics queue. Concurrent
    // sharing is the path that avoids explicit ownership transfers.
    if (concurrent_families != nullptr) {
        ci.sharingMode = VK_SHARING_MODE_CONCURRENT;
        ci.queueFamilyIndexCount = concurrent_family_count;
        ci.pQueueFamilyIndices = concurrent_families;
    }

    if (create_vma_image(allocator, &ci, &image, &allocation) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create AO image");
    }

    VkImageViewCreateInfo view_ci{};
    view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_ci.image = image;
    view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_ci.format = AO_FORMAT;
    view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    if (vkCreateImageView(device, &view_ci, nullptr, &view) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create AO image view");
    }
}

void destroy_ao_image(VkDevice device, VmaAllocator allocator,
                       VkImage& image, VmaAllocation& allocation, VkImageView& view) {
    if (view != VK_NULL_HANDLE) { vkDestroyImageView(device, view, nullptr); view = VK_NULL_HANDLE; }
    destroy_vma_image(allocator, image, allocation);
    image = VK_NULL_HANDLE; allocation = VK_NULL_HANDLE;
}

} // namespace

// ── Images (per-viewport state) ─────────────────────────────────────────────

void AoPass::Images::create(VkDevice device, VmaAllocator allocator,
                             const uint32_t* concurrent_families,
                             uint32_t concurrent_family_count,
                             uint32_t half_w, uint32_t half_h,
                             VkQueue graphics_queue,
                             VkCommandPool graphics_pool) {
    extent = {half_w, half_h};
    create_ao_image(device, allocator, concurrent_families, concurrent_family_count,
                    half_w, half_h, ao_image, ao_memory, ao_view);
    images_need_init = true;

    // Seed the image in SHADER_READ so the global ao_texture descriptor
    // (baked with VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) passes
    // queue-submit layout validation from frame one — even for viewports
    // whose GTAO runs async on the compute queue, and for pp_resources
    // whose owning viewport never runs GTAO at all (camera preview).
    static int ao_image_counter = 0;
    set_image_debug_name(device, ao_image,
        "ao_image#" + std::to_string(ao_image_counter++)
        + "(" + std::to_string(half_w) + "x" + std::to_string(half_h) + ")");
    if (graphics_queue != VK_NULL_HANDLE && graphics_pool != VK_NULL_HANDLE) {
        VkCommandBufferAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.commandPool = graphics_pool;
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount = 1;
        VkCommandBuffer cmd{};
        vkAllocateCommandBuffers(device, &alloc_info, &cmd);

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);

        VkImageMemoryBarrier2 bar{};
        bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        bar.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        bar.srcAccessMask = 0;
        bar.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        bar.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image = ao_image;
        bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &bar;
        vkCmdPipelineBarrier2(cmd, &dep);

        vkEndCommandBuffer(cmd);

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        vkQueueSubmit(graphics_queue, 1, &submit, VK_NULL_HANDLE);
        vkQueueWaitIdle(graphics_queue);
        vkFreeCommandBuffers(device, graphics_pool, 1, &cmd);
    }
}

void AoPass::Images::destroy(VkDevice device, VmaAllocator allocator) {
    invalidate_imgui_texture();
    destroy_ao_image(device, allocator, ao_image, ao_memory, ao_view);
    extent = {};
}

void AoPass::Images::resize(VkDevice device, VmaAllocator allocator,
                             const uint32_t* concurrent_families,
                             uint32_t concurrent_family_count,
                             uint32_t width, uint32_t height,
                             VkQueue graphics_queue,
                             VkCommandPool graphics_pool) {
    if (width == 0 || height == 0) return;

    uint32_t half_w = std::max(1U, width / 2);
    uint32_t half_h = std::max(1U, height / 2);
    if (extent.width == half_w && extent.height == half_h) return;
    vkDeviceWaitIdle(device);

    destroy(device, allocator);
    create(device, allocator, concurrent_families, concurrent_family_count,
           half_w, half_h, graphics_queue, graphics_pool);
}

VkDescriptorSet AoPass::Images::imgui_texture(VkSampler sampler) {
    if (imgui_descriptor == VK_NULL_HANDLE && ao_view != VK_NULL_HANDLE) {
        imgui_descriptor = ImGui_ImplVulkan_AddTexture(
            sampler, ao_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return imgui_descriptor;
}

void AoPass::Images::invalidate_imgui_texture() {
    if (imgui_descriptor != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(imgui_descriptor);
        imgui_descriptor = VK_NULL_HANDLE;
    }
}

// ── AoPass ──────────────────────────────────────────────────────────────────

Result<> AoPass::create(Device& device, const PassResources& resources) {
    device_ = device.handle();
    concurrent_families_ = device.concurrent_queue_families(concurrent_family_count_);
    vk_pipeline_cache_ = resources.pipeline_cache;
    allocator_ = resources.allocator;
    global_set_layout_ = resources.global_set_layout;
    init_graphics_queue_ = resources.graphics_queue;
    init_graphics_pool_ = resources.command_pool;

    VkSamplerCreateInfo sampler_ci{};
    sampler_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_ci.magFilter = VK_FILTER_LINEAR;
    sampler_ci.minFilter = VK_FILTER_LINEAR;
    sampler_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device_, &sampler_ci, nullptr, &ao_sampler_) != VK_SUCCESS) {
        return make_error("Failed to create AO sampler");
    }

    // Blur descriptor set: binding 0 = storage output, binding 1 = sampled input.
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo set_ci{};
    set_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_ci.bindingCount = static_cast<uint32_t>(bindings.size());
    set_ci.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(device_, &set_ci, nullptr, &blur_set_layout_) != VK_SUCCESS) {
        return make_error("Failed to create AO blur descriptor set layout");
    }

    std::array<VkDescriptorSetLayout, 2> set_layouts = {global_set_layout_, blur_set_layout_};
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.size = sizeof(BlurPushConstants);

    VkPipelineLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_ci.setLayoutCount = static_cast<uint32_t>(set_layouts.size());
    layout_ci.pSetLayouts = set_layouts.data();
    layout_ci.pushConstantRangeCount = 1;
    layout_ci.pPushConstantRanges = &push_range;
    if (vkCreatePipelineLayout(device_, &layout_ci, nullptr, &blur_layout_) != VK_SUCCESS) {
        return make_error("Failed to create AO blur pipeline layout");
    }

    auto comp_code = read_spirv(base_path::resolve("shaders/gtao_blur.comp.spv"));
    auto* comp_module = create_shader_module(device_, comp_code);

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
        return make_error("Failed to create AO blur compute pipeline");
    }
    vkDestroyShaderModule(device_, comp_module, nullptr);

    // 1x1 placeholder so the global "ao_texture" binding is valid from frame 1.
    default_images_.create(device_, allocator_,
                           concurrent_families_, concurrent_family_count_, 1, 1);

    if (auto r = create_trace(device, resources); !r) { return r; }

    FJELL_GFX_DEBUG("AO pass created");
    return {};
}

void AoPass::destroy() {
    destroy_trace();
    default_images_.destroy(device_, allocator_);
    if (ao_sampler_ != VK_NULL_HANDLE) { vkDestroySampler(device_, ao_sampler_, nullptr); ao_sampler_ = VK_NULL_HANDLE; }
    if (blur_pipeline_ != VK_NULL_HANDLE) { vkDestroyPipeline(device_, blur_pipeline_, nullptr); blur_pipeline_ = VK_NULL_HANDLE; }
    if (blur_layout_ != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device_, blur_layout_, nullptr); blur_layout_ = VK_NULL_HANDLE; }
    if (blur_set_layout_ != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device_, blur_set_layout_, nullptr); blur_set_layout_ = VK_NULL_HANDLE; }
}

void AoPass::resize(uint32_t width, uint32_t height) {
    default_images_.resize(device_, allocator_,
                           concurrent_families_, concurrent_family_count_,
                           width, height,
                           init_graphics_queue_, init_graphics_pool_);
}

VkDescriptorSet AoPass::imgui_texture() {
    return default_images_.imgui_texture(ao_sampler_);
}

void AoPass::invalidate_imgui_texture() {
    default_images_.invalidate_imgui_texture();
}

void AoPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    auto depth = builder.import_named(ctx, "depth");
    builder.read(depth, ResourceAccess::sampled_compute);

    auto ao = builder.import_named(ctx, "gtao_ao");
    if (enabled_) {
        builder.write(ao, ResourceAccess::storage_write_compute);
        declare_trace(builder, ctx);
        // Enabled path: blur_v is the last node to touch ao, and it
        // declares its own final_layout to SHADER_READ. Nothing to
        // claim here.
    } else {
        // Disabled path: we clear ao_image via vkCmdClearColorImage.
        // This is the only ao node running when disabled — claim the
        // final SHADER_READ layout here. record() emits the matching
        // TRANSFER_DST → SHADER_READ transition inline.
        builder.write(ao, ResourceAccess::transfer_dst);
        builder.final_layout(ao, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                             VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }

    builder.has_side_effects();
}

void AoPass::record(const FrameContext& ctx) {
    auto* imgs = ctx.pp_resources ? ctx.pp_resources->gtao() : nullptr;
    auto& g = imgs != nullptr ? *imgs : default_images_;
    if (g.ao_view == VK_NULL_HANDLE) return;

    FJELL_GPU_ZONE(ctx, "GTAO Trace");
    auto* cmd = ctx.cmd;

    if (!enabled_) {
        // Graph barriered us into TRANSFER_DST. Clear, then emit the
        // matching TRANSFER_DST → SHADER_READ transition to honor the
        // final_layout promise (no follower nodes run when disabled).
        VkClearColorValue clear_value{};
        clear_value.float32[0] = 1.0F;
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, g.ao_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &clear_value, 1, &range);

        VkImageMemoryBarrier2 bar{};
        bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        bar.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        bar.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        // Compute-queue-safe dstStage; semaphore covers the visibility
        // to graphics-side FRAGMENT_SHADER readers. Same reasoning as
        // AoBlurVPass::record's final transition.
        bar.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        bar.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bar.image = g.ao_image;
        bar.subresourceRange = range;
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &bar;
        vkCmdPipelineBarrier2(cmd, &dep);
        return;
    }

    record_trace(ctx, g);
    g.images_need_init = false;
}

void AoPass::collect_exports(DeclareContext& /*ctx*/) {
    // gtao_ao is registered by RenderPipeline::execute() from the active
    // pp_resources (per-viewport ao_image), not here — AoPass only knows
    // about its default placeholder Images, which isn't what any real
    // viewport writes to.
}

// ── GtaoTracePass ───────────────────────────────────────────────────────────

Result<> GtaoTracePass::create_trace(Device& /*device*/, const PassResources& /*resources*/) {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo set_ci{};
    set_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_ci.bindingCount = 1;
    set_ci.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(device_, &set_ci, nullptr, &trace_set_layout_) != VK_SUCCESS) {
        return make_error("Failed to create GTAO trace descriptor set layout");
    }

    std::array<VkDescriptorSetLayout, 2> set_layouts = {global_set_layout_, trace_set_layout_};
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.size = sizeof(GtaoPushConstants);

    VkPipelineLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_ci.setLayoutCount = static_cast<uint32_t>(set_layouts.size());
    layout_ci.pSetLayouts = set_layouts.data();
    layout_ci.pushConstantRangeCount = 1;
    layout_ci.pPushConstantRanges = &push_range;
    if (vkCreatePipelineLayout(device_, &layout_ci, nullptr, &trace_layout_) != VK_SUCCESS) {
        return make_error("Failed to create GTAO trace pipeline layout");
    }

    auto comp_code = read_spirv(base_path::resolve("shaders/gtao.comp.spv"));
    auto* comp_module = create_shader_module(device_, comp_code);

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = comp_module;
    stage.pName = "main";

    VkComputePipelineCreateInfo pipeline_ci{};
    pipeline_ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_ci.stage = stage;
    pipeline_ci.layout = trace_layout_;
    if (vkCreateComputePipelines(device_, vk_pipeline_cache_, 1, &pipeline_ci,
                                  nullptr, &trace_pipeline_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, comp_module, nullptr);
        return make_error("Failed to create GTAO trace compute pipeline");
    }
    vkDestroyShaderModule(device_, comp_module, nullptr);
    return {};
}

void GtaoTracePass::destroy_trace() {
    if (trace_pipeline_ != VK_NULL_HANDLE) { vkDestroyPipeline(device_, trace_pipeline_, nullptr); trace_pipeline_ = VK_NULL_HANDLE; }
    if (trace_layout_ != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device_, trace_layout_, nullptr); trace_layout_ = VK_NULL_HANDLE; }
    if (trace_set_layout_ != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device_, trace_set_layout_, nullptr); trace_set_layout_ = VK_NULL_HANDLE; }
}

void GtaoTracePass::declare_trace(PassBuilder& /*builder*/, const DeclareContext& /*ctx*/) {
    // No additional accesses beyond AoPass::declare.
}

void GtaoTracePass::record_trace(const FrameContext& ctx, Images& g) {
    auto* cmd = ctx.cmd;
    auto global_set = ctx.global_descriptor_set;
    auto target_extent = ctx.target->extent();
    uint32_t width = target_extent.width;
    uint32_t height = target_extent.height;
    uint32_t half_w = g.extent.width;
    uint32_t half_h = g.extent.height;
    uint32_t groups_x = (half_w + TILE_SIZE - 1) / TILE_SIZE;
    uint32_t groups_y = (half_h + TILE_SIZE - 1) / TILE_SIZE;

    FrameCacheBinding b{};
    b.binding = 0;
    b.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b.image.imageView = g.ao_view;
    b.image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorSet set = ctx.descriptor_cache->acquire(trace_set_layout_, std::span{&b, 1});

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, trace_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, trace_layout_,
                            0, 1, &global_set, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, trace_layout_,
                            1, 1, &set, 0, nullptr);

    GtaoPushConstants push{};
    push.radius = radius();
    push.intensity = intensity();
    push.thickness = thickness();
    push.max_pixel_radius = 128.0F;
    push.viewport_width = half_w;
    push.viewport_height = half_h;
    push.full_viewport_width = width;
    push.full_viewport_height = height;

    vkCmdPushConstants(cmd, trace_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);
    vkCmdDispatch(cmd, groups_x, groups_y, 1);
}

PassSetup GtaoTracePass::setup() const {
    return {
        .name = "ao_trace",
        .phase = RenderPhase::GpuDriven,
        .sub_order = 100,
        // .async_hint = AsyncComputeHint::Prefer — infra complete but
        // flipping the hint surfaces a descriptor vs layout race on
        // offscreen#N.resolve_color when post-process passes route to
        // graphics_post_cmd. Pending a proper audit of ImGui-bound
        // descriptor layouts across all three submits.
    };
}

// ── AoBlurHPass ─────────────────────────────────────────────────────────────

Result<> AoBlurHPass::create(Device& /*device*/, const PassResources& /*resources*/) {
    return {};
}

void AoBlurHPass::destroy() {}

void AoBlurHPass::post_create(GpuContext& /*gpu*/, PassRegistry& passes) {
    owner_ = &passes.get<AoPass>("ao_trace");
}

PassSetup AoBlurHPass::setup() const {
    return {
        .name = "ao_blur_h",
        .phase = RenderPhase::GpuDriven,
        .sub_order = 101,
        // .async_hint = AsyncComputeHint::Prefer — see GtaoTracePass.
    };
}

void AoBlurHPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    if (owner_ == nullptr || !owner_->enabled()) { return; }

    auto ao = builder.import_named(ctx, "gtao_ao");
    builder.read(ao, ResourceAccess::sampled_compute);

    TextureDesc blur_desc{};
    blur_desc.format = VK_FORMAT_R8_UNORM;
    blur_desc.size_class = SizeClass::viewport;
    blur_desc.viewport_divisor = 2;
    auto blur = builder.create("gtao_blur", blur_desc);
    builder.write(blur, ResourceAccess::storage_write_compute);
}

void AoBlurHPass::record(const FrameContext& ctx) {
    if (owner_ == nullptr || !owner_->enabled()) { return; }
    if (ctx.registry == nullptr) { return; }

    auto* pp = ctx.pp_resources;
    auto* imgs = pp != nullptr ? pp->gtao() : nullptr;
    auto& g = imgs != nullptr ? *imgs : owner_->images();
    if (g.ao_view == VK_NULL_HANDLE) { return; }

    FJELL_GPU_ZONE(ctx, "GTAO Blur H");

    const auto* blur_res = ctx.registry->find_image("gtao_blur");
    if (blur_res == nullptr || blur_res->image == VK_NULL_HANDLE) { return; }

    auto* cmd = ctx.cmd;
    auto global_set = ctx.global_descriptor_set;
    uint32_t half_w = g.extent.width;
    uint32_t half_h = g.extent.height;
    uint32_t groups_x = (half_w + TILE_SIZE - 1) / TILE_SIZE;
    uint32_t groups_y = (half_h + TILE_SIZE - 1) / TILE_SIZE;

    std::array<FrameCacheBinding, 2> bindings{};
    bindings[0].binding = 0;
    bindings[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[0].image.imageView = blur_res->view;
    bindings[0].image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    bindings[1].binding = 1;
    bindings[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].image.sampler = owner_->ao_sampler();
    bindings[1].image.imageView = g.ao_view;
    bindings[1].image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorSet set = ctx.descriptor_cache->acquire(owner_->blur_set_layout(), bindings);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, owner_->blur_pipeline());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, owner_->blur_pipeline_layout(),
                            0, 1, &global_set, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, owner_->blur_pipeline_layout(),
                            1, 1, &set, 0, nullptr);

    BlurPushConstants push{};
    push.viewport_width = half_w;
    push.viewport_height = half_h;
    push.direction = 0;
    push.depth_threshold = 0.1F;

    vkCmdPushConstants(cmd, owner_->blur_pipeline_layout(), VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);
    vkCmdDispatch(cmd, groups_x, groups_y, 1);
}

// ── AoBlurVPass ─────────────────────────────────────────────────────────────

Result<> AoBlurVPass::create(Device& /*device*/, const PassResources& /*resources*/) {
    return {};
}

void AoBlurVPass::destroy() {}

void AoBlurVPass::post_create(GpuContext& /*gpu*/, PassRegistry& passes) {
    owner_ = &passes.get<AoPass>("ao_trace");
}

PassSetup AoBlurVPass::setup() const {
    return {
        .name = "ao_blur_v",
        .phase = RenderPhase::GpuDriven,
        .sub_order = 102,
        // .async_hint = AsyncComputeHint::Prefer — see GtaoTracePass.
    };
}

void AoBlurVPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    if (owner_ == nullptr || !owner_->enabled()) { return; }

    // Declare the same virtual transient AoBlurHPass declared — the
    // graph dedupes virtual resources by name so both passes resolve to
    // the same TrackedImage. This lets the DAG sort see the blur_h →
    // blur_v edge and keeps blur_h alive through the pass-culling walk.
    TextureDesc blur_desc{};
    blur_desc.format = VK_FORMAT_R8_UNORM;
    blur_desc.size_class = SizeClass::viewport;
    blur_desc.viewport_divisor = 2;
    auto blur = builder.create("gtao_blur", blur_desc);
    builder.read(blur, ResourceAccess::sampled_compute);

    auto ao = builder.import_named(ctx, "gtao_ao");
    builder.write(ao, ResourceAccess::storage_write_compute);
    // blur_v is the last node to touch ao — claim the final SHADER_READ
    // layout so the graph's cross-frame tracker matches the actual state
    // (mesh.frag's pre-baked "ao_texture" global descriptor expects
    // SHADER_READ). record() emits the matching GENERAL → SHADER_READ
    // transition inline after its dispatch.
    builder.final_layout(ao, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

void AoBlurVPass::record(const FrameContext& ctx) {
    if (owner_ == nullptr || !owner_->enabled()) { return; }
    if (ctx.registry == nullptr) { return; }

    auto* pp = ctx.pp_resources;
    auto* imgs = pp != nullptr ? pp->gtao() : nullptr;
    auto& g = imgs != nullptr ? *imgs : owner_->images();
    if (g.ao_view == VK_NULL_HANDLE) { return; }

    FJELL_GPU_ZONE(ctx, "GTAO Blur V");

    const auto* blur_res = ctx.registry->find_image("gtao_blur");
    if (blur_res == nullptr || blur_res->image == VK_NULL_HANDLE) { return; }

    auto* cmd = ctx.cmd;
    auto global_set = ctx.global_descriptor_set;
    uint32_t half_w = g.extent.width;
    uint32_t half_h = g.extent.height;
    uint32_t groups_x = (half_w + TILE_SIZE - 1) / TILE_SIZE;
    uint32_t groups_y = (half_h + TILE_SIZE - 1) / TILE_SIZE;

    std::array<FrameCacheBinding, 2> bindings{};
    bindings[0].binding = 0;
    bindings[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[0].image.imageView = g.ao_view;
    bindings[0].image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    bindings[1].binding = 1;
    bindings[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].image.sampler = owner_->ao_sampler();
    bindings[1].image.imageView = blur_res->view;
    bindings[1].image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorSet set = ctx.descriptor_cache->acquire(owner_->blur_set_layout(), bindings);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, owner_->blur_pipeline());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, owner_->blur_pipeline_layout(),
                            0, 1, &global_set, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, owner_->blur_pipeline_layout(),
                            1, 1, &set, 0, nullptr);

    BlurPushConstants push{};
    push.viewport_width = half_w;
    push.viewport_height = half_h;
    push.direction = 1;
    push.depth_threshold = 0.1F;

    vkCmdPushConstants(cmd, owner_->blur_pipeline_layout(), VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);
    vkCmdDispatch(cmd, groups_x, groups_y, 1);

    // Final ao_image transition to SHADER_READ — honors AoPass::declare's
    // final_layout() promise and satisfies the mesh.frag descriptor's
    // baked SHADER_READ layout.
    //
    // dstStage is COMPUTE_SHADER, not FRAGMENT_SHADER: when this pass
    // runs on the compute queue the fragment stage isn't part of the
    // queue's supported set. The cross-queue visibility to mesh.frag is
    // provided by the timeline-semaphore wait in submit_and_present,
    // which upgrades the dependency to FRAGMENT_SHADER on the graphics
    // side. Layout transitions under CONCURRENT sharing happen in
    // whichever CB emits the barrier, so this one legal on compute.
    VkImageMemoryBarrier2 bar{};
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    bar.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    bar.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    bar.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    bar.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    bar.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bar.image = g.ao_image;
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &bar;
    vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace fjell
