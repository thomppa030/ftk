#include "renderer/pass/taa_resolve_pass.hpp"
#include "core/profiler.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/gpu/vk_utils.hpp"
#include "renderer/gpu/vma_image.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/post_process_resources.hpp"
#include "renderer/resource_registry.hpp"
#include "renderer/resources/shader_utils.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#ifdef FJELL_ENABLE_TRACY
#include <tracy/TracyVulkan.hpp>
#endif

#include <array>
#include <stdexcept>

namespace fjell {

// ── Images (per-viewport state) ─────────────────────────────────────────────

void TaaResolvePass::Images::create(VkDevice device, VmaAllocator allocator,
                                     VkDescriptorSetLayout set_layout,
                                     uint32_t width, uint32_t height) {
    extent = {width, height};
    write_index = 0;
    needs_init = true;

    for (int i = 0; i < 2; ++i) {
        VkImageCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        ci.extent = {width, height, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (create_vma_image(allocator, &ci, &history[i], &history_memory[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create TAA history image");
        }

        VkImageViewCreateInfo view_ci{};
        view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_ci.image = history[i];
        view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_ci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device, &view_ci, nullptr, &history_view[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create TAA history view");
        }
    }

    // Descriptor pool: 4 image descriptors per set (current, history, velocity, output) × frames
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    // 4 samplers per set: current_color, history, velocity, depth.
    pool_size.descriptorCount = MAX_FRAMES_IN_FLIGHT * 4;

    VkDescriptorPoolSize storage_size{};
    storage_size.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    storage_size.descriptorCount = MAX_FRAMES_IN_FLIGHT;

    std::array pool_sizes = {pool_size, storage_size};

    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.maxSets = MAX_FRAMES_IN_FLIGHT;
    pool_ci.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
    pool_ci.pPoolSizes = pool_sizes.data();
    if (vkCreateDescriptorPool(device, &pool_ci, nullptr, &pool) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create TAA descriptor pool");
    }

    std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> layouts{};
    layouts.fill(set_layout);
    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = pool;
    alloc_info.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    alloc_info.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(device, &alloc_info, sets) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate TAA descriptor sets");
    }
}

void TaaResolvePass::Images::destroy(VkDevice device, VmaAllocator allocator) {
    if (pool != VK_NULL_HANDLE) { vkDestroyDescriptorPool(device, pool, nullptr); pool = VK_NULL_HANDLE; }
    for (int i = 0; i < 2; ++i) {
        if (history_view[i] != VK_NULL_HANDLE) { vkDestroyImageView(device, history_view[i], nullptr); history_view[i] = VK_NULL_HANDLE; }
        destroy_vma_image(allocator, history[i], history_memory[i]);
        history[i] = VK_NULL_HANDLE;
        history_memory[i] = VK_NULL_HANDLE;
    }
    extent = {};
    for (auto& s : sets) s = VK_NULL_HANDLE;
}

void TaaResolvePass::Images::resize(VkDevice device, VmaAllocator allocator,
                                     VkDescriptorSetLayout set_layout,
                                     uint32_t width, uint32_t height) {
    if (extent.width == width && extent.height == height) return;
    vkDeviceWaitIdle(device);
    destroy(device, allocator);
    if (width > 0 && height > 0) create(device, allocator, set_layout, width, height);
}

// ── TaaResolvePass (singleton) ─────────────────────────────────────────────

Result<> TaaResolvePass::create(Device& device, const PassResources& resources) {
    device_ = device.handle();
    vk_pipeline_cache_ = resources.pipeline_cache;
    allocator_ = resources.allocator;

    sampler_ = vk_utils::create_linear_clamp_sampler(device_);

    // Descriptor set layout:
    // binding 0 = current color (sampled)
    // binding 1 = history color (sampled)
    // binding 2 = velocity (sampled)
    // binding 3 = resolve output (storage image)
    // binding 4 = depth (sampled) — for closest-depth velocity sampling
    std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
    for (uint32_t i = 0; i < 3; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[4].binding = 4;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_ci.bindingCount = static_cast<uint32_t>(bindings.size());
    layout_ci.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(device_, &layout_ci, nullptr, &set_layout_) != VK_SUCCESS) {
        return make_error("Failed to create TAA descriptor layout");
    }

    // Pipeline layout with push constants
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset = 0;
    push_range.size = 4 * sizeof(float); // texel_size.xy + blend_factor + pad

    VkPipelineLayoutCreateInfo pipe_layout_ci{};
    pipe_layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipe_layout_ci.setLayoutCount = 1;
    pipe_layout_ci.pSetLayouts = &set_layout_;
    pipe_layout_ci.pushConstantRangeCount = 1;
    pipe_layout_ci.pPushConstantRanges = &push_range;
    if (vkCreatePipelineLayout(device_, &pipe_layout_ci, nullptr, &layout_) != VK_SUCCESS) {
        return make_error("Failed to create TAA pipeline layout");
    }

    // Compute pipeline
    auto comp_code = read_spirv(base_path::resolve("shaders/taa_resolve.comp.spv"));
    auto comp_module = create_shader_module(device_, comp_code);

    VkComputePipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = comp_module;
    ci.stage.pName = "main";
    ci.layout = layout_;

    if (vkCreateComputePipelines(device_, vk_pipeline_cache_, 1, &ci, nullptr, &pipeline_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, comp_module, nullptr);
        return make_error("Failed to create TAA compute pipeline");
    }
    vkDestroyShaderModule(device_, comp_module, nullptr);

    FJELL_GFX_DEBUG("TAA resolve pass created (compute pipeline)");
    return {};
}

void TaaResolvePass::destroy() {
    default_images_.destroy(device_, allocator_);
    if (pipeline_ != VK_NULL_HANDLE) { vkDestroyPipeline(device_, pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE; }
    if (layout_ != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device_, layout_, nullptr); layout_ = VK_NULL_HANDLE; }
    if (set_layout_ != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr); set_layout_ = VK_NULL_HANDLE; }
    if (sampler_ != VK_NULL_HANDLE) { vkDestroySampler(device_, sampler_, nullptr); sampler_ = VK_NULL_HANDLE; }
}

void TaaResolvePass::resize(uint32_t width, uint32_t height) {
    default_images_.resize(device_, allocator_, set_layout_, width, height);
}

VkImageView TaaResolvePass::output_view() const {
    // The write target from the current frame becomes the read source next frame,
    // but downstream passes this frame read the just-written image.
    return default_images_.history_view[default_images_.write_index];
}

VkImage TaaResolvePass::output_image() const {
    return default_images_.history[default_images_.write_index];
}

PassSetup TaaResolvePass::setup() const {
    return {
        .name = "taa_resolve",
        .phase = RenderPhase::PostProcess,
        .sub_order = 150, // after volumetric fog (100), before bloom (200)
    };
}

void TaaResolvePass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    auto color = builder.import_named(ctx, "color");
    auto velocity = builder.import_named(ctx, "velocity");
    auto depth = builder.import_named(ctx, "depth");
    builder.read(color, ResourceAccess::sampled_compute);
    builder.read(velocity, ResourceAccess::sampled_compute);
    builder.read(depth, ResourceAccess::sampled_compute);
    // History is pass-private; tonemap samples it through a non-catalog
    // view. Flag side-effecting so culling keeps this alive.
    builder.has_side_effects();
}

void TaaResolvePass::record(const FrameContext& ctx) {
    FJELL_PROFILE_SCOPE_N("record_taa");
    auto* imgs = ctx.pp_resources ? ctx.pp_resources->taa() : nullptr;
    record_into(ctx, imgs ? *imgs : default_images_);
}

void TaaResolvePass::record_into(const FrameContext& ctx, Images& imgs) {
    if (imgs.extent.width == 0 || !ctx.target) return;

#ifdef FJELL_ENABLE_TRACY
    TracyVkZone(static_cast<tracy::VkCtx*>(ctx.tracy_ctx), ctx.cmd, "TAA Resolve");
#endif

    auto cmd = ctx.cmd;
    uint32_t write_idx = imgs.write_index;
    uint32_t read_idx = 1 - write_idx;

    // On first frame, initialize history layout and use full passthrough
    bool first_frame = imgs.needs_init;
    if (imgs.needs_init) {
        for (int i = 0; i < 2; ++i) {
            vk_utils::transition_image(cmd, imgs.history[i],
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }
        imgs.needs_init = false;
    }

    // Transition write target to GENERAL for storage write
    vk_utils::transition_image(cmd, imgs.history[write_idx],
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    // Update descriptors:
    // binding 0 = current color (scene color view)
    // binding 1 = history (read index)
    // binding 2 = velocity
    // binding 3 = output (write index, storage)
    auto set = imgs.sets[ctx.frame_index];

    vk_utils::write_image_descriptor(device_, set, 0,
        ctx.target->color_view(), sampler_);
    vk_utils::write_image_descriptor(device_, set, 1,
        imgs.history_view[read_idx], sampler_);
    vk_utils::write_image_descriptor(device_, set, 2,
        ctx.target->velocity_view(), sampler_);
    // Depth is in DEPTH_STENCIL_READ_ONLY layout (not the plain SHADER_READ
    // the helper defaults to). The framegraph already transitions it to this
    // layout for sampled-compute reads.
    vk_utils::write_image_descriptor(device_, set, 4,
        ctx.target->depth_view(), sampler_,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);

    // Storage image write (binding 3)
    {
        VkDescriptorImageInfo storage_info{};
        storage_info.imageView = imgs.history_view[write_idx];
        storage_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = 3;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        write.descriptorCount = 1;
        write.pImageInfo = &storage_info;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }

    // Dispatch
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_,
                            0, 1, &set, 0, nullptr);

    struct {
        float texel_x, texel_y;
        float blend_factor;
        float pad;
    } push{};
    push.texel_x = 1.0f / static_cast<float>(imgs.extent.width);
    push.texel_y = 1.0f / static_cast<float>(imgs.extent.height);
    push.blend_factor = first_frame ? 1.0f : 0.05f;

    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);

    uint32_t groups_x = (imgs.extent.width + 7) / 8;
    uint32_t groups_y = (imgs.extent.height + 7) / 8;
    vkCmdDispatch(cmd, groups_x, groups_y, 1);

    // Transition write target back to SHADER_READ_ONLY for downstream sampling
    vk_utils::transition_image(cmd, imgs.history[write_idx],
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    // Flip ping-pong
    imgs.write_index = read_idx;
}

} // namespace fjell
