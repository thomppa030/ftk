#include "renderer/pass/tonemap_pass.hpp"
#include "renderer/pass/bloom_pass.hpp"
#include "renderer/pass/exposure_pass.hpp"
#include "renderer/pass/taa_resolve_pass.hpp"
#include "renderer/pass_builder.hpp"

#include <algorithm>
#include <cmath>
#include "renderer/gpu/vma_image.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass_registry.hpp"
#include "renderer/post_process_resources.hpp"
#include "renderer/resource_registry.hpp"
#include "renderer/resources/shader_utils.hpp"
#include "renderer/gpu/vk_utils.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#include <imgui_impl_vulkan.h>

#include <array>
#include <stdexcept>

namespace fjell {

// ── Images (per-viewport state) ─────────────────────────────────────────────

void TonemapPass::Images::create(VkDevice device, VmaAllocator allocator,
                                  VkDescriptorSetLayout set_layout,
                                  uint32_t width, uint32_t height) {
    extent = {width, height};

    // Create output image (LDR sRGB)
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8G8B8A8_SRGB;
    ci.extent = {width, height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (create_vma_image(allocator, &ci, &image, &memory) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create tonemap output image");
    }

    VkImageViewCreateInfo view_ci{};
    view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_ci.image = image;
    view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_ci.format = VK_FORMAT_R8G8B8A8_SRGB;
    view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device, &view_ci, nullptr, &view) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create tonemap output view");
    }

    // Create own descriptor pool + sets (2 samplers per set: scene + bloom)
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = MAX_FRAMES_IN_FLIGHT * 2;

    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.maxSets = MAX_FRAMES_IN_FLIGHT;
    pool_ci.poolSizeCount = 1;
    pool_ci.pPoolSizes = &pool_size;
    if (vkCreateDescriptorPool(device, &pool_ci, nullptr, &pool) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create tonemap images descriptor pool");
    }

    std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> layouts{};
    layouts.fill(set_layout);
    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = pool;
    alloc_info.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    alloc_info.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(device, &alloc_info, input_sets) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate tonemap images descriptor sets");
    }
}

void TonemapPass::Images::destroy(VkDevice device, VmaAllocator allocator) {
    invalidate_imgui_texture();
    if (pool != VK_NULL_HANDLE) { vkDestroyDescriptorPool(device, pool, nullptr); pool = VK_NULL_HANDLE; }
    if (view != VK_NULL_HANDLE) { vkDestroyImageView(device, view, nullptr); view = VK_NULL_HANDLE; }
    destroy_vma_image(allocator, image, memory);
    image = VK_NULL_HANDLE; memory = VK_NULL_HANDLE;
    extent = {};
    for (auto& s : input_sets) s = VK_NULL_HANDLE;
}

void TonemapPass::Images::resize(VkDevice device, VmaAllocator allocator,
                                  VkDescriptorSetLayout set_layout,
                                  uint32_t width, uint32_t height) {
    if (extent.width == width && extent.height == height) return;
    vkDeviceWaitIdle(device);

    destroy(device, allocator);
    if (width > 0 && height > 0) create(device, allocator, set_layout, width, height);
}

VkDescriptorSet TonemapPass::Images::imgui_texture(VkSampler sampler) {
    if (imgui_descriptor == VK_NULL_HANDLE && view != VK_NULL_HANDLE) {
        imgui_descriptor = ImGui_ImplVulkan_AddTexture(
            sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return imgui_descriptor;
}

void TonemapPass::Images::invalidate_imgui_texture() {
    if (imgui_descriptor != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(imgui_descriptor);
        imgui_descriptor = VK_NULL_HANDLE;
    }
}

// ── TonemapPass (singleton) ─────────────────────────────────────────────────

Result<> TonemapPass::create(Device& device, const PassResources& resources) {
    device_ = device.handle();
    vk_pipeline_cache_ = resources.pipeline_cache;
    allocator_ = resources.allocator;

    input_sampler_ = vk_utils::create_linear_clamp_sampler(device_);
    sampler_ = vk_utils::create_linear_clamp_sampler(device_);

    // Descriptor set layout: binding 0 = HDR scene, binding 1 = bloom mip
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_ci.bindingCount = static_cast<uint32_t>(bindings.size());
    layout_ci.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(device_, &layout_ci, nullptr, &set_layout_) != VK_SUCCESS) {
        return make_error("Failed to create tonemap descriptor layout");
    }

    // Pipeline layout with push constants for bloom params
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push_range.offset = 0;
    push_range.size = 3 * sizeof(float);

    VkPipelineLayoutCreateInfo pipe_layout_ci{};
    pipe_layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipe_layout_ci.setLayoutCount = 1;
    pipe_layout_ci.pSetLayouts = &set_layout_;
    pipe_layout_ci.pushConstantRangeCount = 1;
    pipe_layout_ci.pPushConstantRanges = &push_range;
    if (vkCreatePipelineLayout(device_, &pipe_layout_ci, nullptr, &layout_) != VK_SUCCESS) {
        return make_error("Failed to create tonemap pipeline layout");
    }

    // Pipeline
    auto vert_module = create_shader_module(device_, read_spirv(base_path::resolve("shaders/depth_viz.vert.spv")));
    auto frag_module = create_shader_module(device_, read_spirv(base_path::resolve("shaders/tonemap.frag.spv")));

    pipeline_ = vk_utils::create_fullscreen_pipeline({
        .device = device_,
        .layout = layout_,
        .vert_module = vert_module,
        .frag_module = frag_module,
        .color_format = VK_FORMAT_R8G8B8A8_SRGB,
        .pipeline_cache = vk_pipeline_cache_,
    });

    vkDestroyShaderModule(device_, frag_module, nullptr);
    vkDestroyShaderModule(device_, vert_module, nullptr);

    FJELL_GFX_DEBUG("Tonemap pass created");
    return {};
}

void TonemapPass::destroy() {
    default_images_.destroy(device_, allocator_);

    if (pipeline_ != VK_NULL_HANDLE) { vkDestroyPipeline(device_, pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE; }
    if (layout_ != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device_, layout_, nullptr); layout_ = VK_NULL_HANDLE; }
    if (set_layout_ != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr); set_layout_ = VK_NULL_HANDLE; }
    if (input_sampler_ != VK_NULL_HANDLE) { vkDestroySampler(device_, input_sampler_, nullptr); input_sampler_ = VK_NULL_HANDLE; }
    if (sampler_ != VK_NULL_HANDLE) { vkDestroySampler(device_, sampler_, nullptr); sampler_ = VK_NULL_HANDLE; }
}

void TonemapPass::resize(uint32_t width, uint32_t height) {
    default_images_.resize(device_, allocator_, set_layout_, width, height);
}

VkDescriptorSet TonemapPass::imgui_texture() {
    return default_images_.imgui_texture(sampler_);
}

void TonemapPass::invalidate_imgui_texture() {
    default_images_.invalidate_imgui_texture();
}

void TonemapPass::prepare_frame(const FrameContext& ctx, GpuContext& /*gpu*/) {
    if (!ctx.passes) return;

    // Enable/disable histogram pass based on auto-exposure toggle
    auto* exposure_pass = ctx.passes->try_get<ExposurePass>("exposure");
    if (exposure_pass) {
        exposure_pass->set_enabled(auto_exposure_);
    }

    if (!auto_exposure_ || !exposure_pass || !exposure_pass->enabled()) return;

    // Read average luminance from the previous frame's buffer
    uint32_t prev_frame = (ctx.frame_index + MAX_FRAMES_IN_FLIGHT - 1) % MAX_FRAMES_IN_FLIGHT;
    float avg_lum = exposure_pass->average_luminance(prev_frame);

    // EV100 from average luminance (ISO 100 standard)
    // EV100 = log2(avg_luminance / 0.125)  (where 0.125 = 12.5% middle grey)
    float target_ev = -std::log2(std::max(avg_lum, 1e-5f) / 0.125f);

    // Clamp to allowed range
    target_ev = std::clamp(target_ev, min_ev_, max_ev_);

    // Temporal adaptation: lerp toward target
    float speed = adaptation_speed_ * ctx.delta_time;
    exposure_ += (target_ev - exposure_) * std::min(speed, 1.0f);
}

void TonemapPass::record(const FrameContext& ctx) {
    record_into(ctx, default_images_, source_view_, VK_NULL_HANDLE, 0.3f, exposure_);
}

void TonemapPass::record_into(const FrameContext& ctx, Images& imgs, VkImageView source,
                              VkImageView bloom_view, float bloom_intensity,
                              float exposure) {
    if (imgs.view == VK_NULL_HANDLE || source == VK_NULL_HANDLE) return;

    auto cmd = ctx.cmd;

    // Update input descriptors
    vk_utils::write_image_descriptor(device_, imgs.input_sets[ctx.frame_index], 0,
                                     source, input_sampler_);

    // Write bloom descriptor (binding 1) — use a 1x1 black texture if no bloom
    VkImageView bloom = bloom_view != VK_NULL_HANDLE ? bloom_view : source;
    float has_bloom = bloom_view != VK_NULL_HANDLE ? 1.0f : 0.0f;
    vk_utils::write_image_descriptor(device_, imgs.input_sets[ctx.frame_index], 1,
                                     bloom, input_sampler_);

    // Transition output to writable
    vk_utils::prepare_color_attachment(cmd, imgs.image);

    // Render
    VkRenderingAttachmentInfo color_attachment{};
    color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color_attachment.imageView = imgs.view;
    color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.extent = imgs.extent;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color_attachment;

    vkCmdBeginRendering(cmd, &ri);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

    VkViewport vp{};
    vp.width = static_cast<float>(imgs.extent.width);
    if (ctx.flip_tonemap_y) {
        // Negative-height viewport flips the image vertically so render-to-texture
        // outputs have V=0 at bottom (standard texture convention).
        vp.y = static_cast<float>(imgs.extent.height);
        vp.height = -static_cast<float>(imgs.extent.height);
    } else {
        vp.height = static_cast<float>(imgs.extent.height);
    }
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D sc{};
    sc.extent = imgs.extent;
    vkCmdSetScissor(cmd, 0, 1, &sc);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_,
                            0, 1, &imgs.input_sets[ctx.frame_index], 0, nullptr);

    struct { float intensity; float active; float ev; } push{bloom_intensity, has_bloom, exposure};
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);

    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRendering(cmd);

    // Transition to readable
    vk_utils::finish_color_attachment(cmd, imgs.image);
}

PassSetup TonemapPass::setup() const {
    return {
        .name = "tonemap",
        .phase = RenderPhase::PostProcess,
        .sub_order = 300,
        .reads = {{"scene_color", ImageUsage::shader_read}},
        .writes = {{"tonemapped", ImageUsage::shader_read}},
    };
}

void TonemapPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    auto color = builder.import_named(ctx, "color");
    builder.read(color, ResourceAccess::sampled_fragment);
    // Tonemap's alternate color sources (TAA history, bloom mip 0) live
    // on pass-private resources and are resolved inside record().
}

void TonemapPass::build(FrameGraph& graph, ResourceRegistry& /*registry*/,
                         const FrameContext& ctx) {
    auto* target = ctx.target;
    if (target == nullptr) { return; }

    PassBuilder builder;
    auto color = builder.import("color", target->color_image(), target->color_view());
    builder.read(color, ResourceAccess::sampled_fragment);

    graph.submit_declared_pass("tonemap", builder,
        [this, ctx](VkCommandBuffer cb) {
            FrameContext local = ctx;
            local.cmd = cb;
            if (local.pp_resources && local.pp_resources->tonemap()) {
                VkImageView bloom_view = VK_NULL_HANDLE;
                if (local.passes) {
                    auto* bloom = local.passes->try_get<BloomPass>("bloom");
                    if (bloom && bloom->enabled() && local.pp_resources->bloom()) {
                        bloom_view = local.pp_resources->bloom()->mips[0].view;
                    }
                }
                // Use TAA-resolved color when available, otherwise raw scene color.
                VkImageView color_source = local.target->color_view();
                if (auto* taa_imgs = local.pp_resources->taa()) {
                    // After TAA record_into() flips write_index, the just-
                    // written output is at [1 - write_index] (the old write
                    // target).
                    uint32_t output_idx = 1 - taa_imgs->write_index;
                    if (taa_imgs->history_view[output_idx] != VK_NULL_HANDLE) {
                        color_source = taa_imgs->history_view[output_idx];
                    }
                }
                record_into(local, *local.pp_resources->tonemap(),
                            color_source, bloom_view, 0.3f, exposure_);
            }
        });
}

} // namespace fjell
