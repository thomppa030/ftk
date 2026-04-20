#include "renderer/pass/volumetric_fog_pass.hpp"
#include "renderer/frame_descriptor_cache.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/post_process_resources.hpp"
#include "renderer/resource_registry.hpp"
#include "renderer/gpu/vma_image.hpp"
#include "renderer/resources/shader_utils.hpp"
#include "scene/light_node.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace fjell {

struct FogPushConstants {
    glm::uvec4 volume_dims;   // xyz = froxel grid, w = frame_index
    float near_plane;
    float far_plane;
    float density;
    float height;
    float height_falloff;
    float scattering;
    float anisotropy;
    float temporal_blend;
    uint32_t viewport_width;
    uint32_t viewport_height;
    float fog_color_r;
    float fog_color_g;
    float fog_color_b;
    float jitter_x;           // precomputed Halton jitter
    float jitter_y;
    float jitter_z;
    float padding;
};

// Halton sequence (computed on CPU, passed via push constants)
static float halton(int index, int base) {
    float result = 0.0f;
    float f = 1.0f / static_cast<float>(base);
    int i = index;
    while (i > 0) {
        result += f * static_cast<float>(i % base);
        i /= base;
        f /= static_cast<float>(base);
    }
    return result;
}

// ── Helpers ──────────────────────────────────────────────────────────────────

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

    VkPipeline pipeline;
    if (vkCreateComputePipelines(device, pipeline_cache, 1, &ci,
                                  nullptr, &pipeline) != VK_SUCCESS) {
        vkDestroyShaderModule(device, module, nullptr);
        throw std::runtime_error("Failed to create volumetric fog compute pipeline");
    }
    vkDestroyShaderModule(device, module, nullptr);
    return pipeline;
}

static VkPipelineLayout create_pipeline_layout(
        VkDevice device,
        const VkDescriptorSetLayout* set_layouts, uint32_t set_count,
        uint32_t push_size) {
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset = 0;
    push_range.size = push_size;

    VkPipelineLayoutCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    ci.setLayoutCount = set_count;
    ci.pSetLayouts = set_layouts;
    ci.pushConstantRangeCount = 1;
    ci.pPushConstantRanges = &push_range;

    VkPipelineLayout layout;
    if (vkCreatePipelineLayout(device, &ci, nullptr, &layout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create volumetric fog pipeline layout");
    }
    return layout;
}

// ── VolumeImage helpers ─────────────────────────────────────────────────────

VolumetricFogPass::VolumeImage VolumetricFogPass::create_volume_image(
        VkDevice device, VmaAllocator allocator,
        uint32_t w, uint32_t h, uint32_t d) {
    VolumeImage img{};

    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_3D;
    ci.format = VOLUME_FORMAT;
    ci.extent = {w, h, d};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (create_vma_image(allocator, &ci, &img.image, &img.memory) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create fog volume image");
    }

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_3D;
    vi.format = VOLUME_FORMAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    if (vkCreateImageView(device, &vi, nullptr, &img.view) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create fog volume image view");
    }

    return img;
}

void VolumetricFogPass::destroy_volume_image(VkDevice device, VmaAllocator allocator, VolumeImage& img) {
    if (img.view) vkDestroyImageView(device, img.view, nullptr);
    destroy_vma_image(allocator, img.image, img.memory);
    img = {};
}

// ── Images ──────────────────────────────────────────────────────────────────

void VolumetricFogPass::Images::create(VkDevice device, VmaAllocator allocator,
                                        uint32_t vol_w, uint32_t vol_h) {
    volume_extent = {vol_w, vol_h, DEPTH_SLICES};
    volumes_need_init = true;
    ping_pong = 0;
    frame_counter = 0;

    scatter_volumes[0] = create_volume_image(device, allocator, vol_w, vol_h, DEPTH_SLICES);
    scatter_volumes[1] = create_volume_image(device, allocator, vol_w, vol_h, DEPTH_SLICES);
    integrated_volume = create_volume_image(device, allocator, vol_w, vol_h, DEPTH_SLICES);
}

void VolumetricFogPass::Images::destroy(VkDevice device, VmaAllocator allocator) {
    invalidate_imgui_texture();
    destroy_volume_image(device, allocator, scatter_volumes[0]);
    destroy_volume_image(device, allocator, scatter_volumes[1]);
    destroy_volume_image(device, allocator, integrated_volume);
    volume_extent = {};
}

void VolumetricFogPass::Images::resize(VkDevice device, VmaAllocator allocator,
                                        uint32_t screen_w, uint32_t screen_h) {
    uint32_t vol_w = std::max(1u, screen_w / SCREEN_DIVISOR);
    uint32_t vol_h = std::max(1u, screen_h / SCREEN_DIVISOR);

    if (vol_w == volume_extent.width && vol_h == volume_extent.height) return;

    // Skip resize if volumes already exist and change is tiny (< 4 texels)
    // This prevents thrashing during interactive viewport dragging
    if (volume_extent.width > 1) {
        int dw = static_cast<int>(vol_w) - static_cast<int>(volume_extent.width);
        int dh = static_cast<int>(vol_h) - static_cast<int>(volume_extent.height);
        if (std::abs(dw) < 4 && std::abs(dh) < 4) return;
    }
    vkDeviceWaitIdle(device);

    destroy(device, allocator);
    create(device, allocator, vol_w, vol_h);
}

VkDescriptorSet VolumetricFogPass::Images::imgui_texture(VkSampler sampler) {
    if (imgui_descriptor == VK_NULL_HANDLE && integrated_volume.view != VK_NULL_HANDLE) {
        imgui_descriptor = ImGui_ImplVulkan_AddTexture(
            sampler, integrated_volume.view,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return imgui_descriptor;
}

void VolumetricFogPass::Images::invalidate_imgui_texture() {
    if (imgui_descriptor != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(imgui_descriptor);
        imgui_descriptor = VK_NULL_HANDLE;
    }
}

// ── VolumetricFogPass (singleton) ───────────────────────────────────────────

Result<> VolumetricFogPass::create(Device& device, const PassResources& resources) {
    device_ = device.handle();
    vk_pipeline_cache_ = resources.pipeline_cache;
    allocator_ = resources.allocator;
    auto global_set_layout = resources.global_set_layout;

    // Linear sampler for trilinear 3D texture sampling
    VkSamplerCreateInfo sampler_ci{};
    sampler_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_ci.magFilter = VK_FILTER_LINEAR;
    sampler_ci.minFilter = VK_FILTER_LINEAR;
    sampler_ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device_, &sampler_ci, nullptr, &linear_sampler_) != VK_SUCCESS) {
        return make_error("Failed to create fog sampler");
    }

    // ── Inject set layout: binding 0 = storage image (scatter write) ──
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
        vkCreateDescriptorSetLayout(device_, &ci, nullptr, &inject_set_layout_);

        std::array layouts{global_set_layout, inject_set_layout_};
        inject_pipeline_layout_ = create_pipeline_layout(
            device_, layouts.data(), static_cast<uint32_t>(layouts.size()),
            sizeof(FogPushConstants));
        inject_pipeline_ = create_compute_pipeline(
            device_, inject_pipeline_layout_, "shaders/fog_inject.comp.spv", vk_pipeline_cache_);
    }

    // ── Temporal set layout: binding 0 = storage image (current rw), binding 1 = sampler3D (prev) ──
    {
        std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ci.bindingCount = static_cast<uint32_t>(bindings.size());
        ci.pBindings = bindings.data();
        vkCreateDescriptorSetLayout(device_, &ci, nullptr, &temporal_set_layout_);

        std::array layouts{global_set_layout, temporal_set_layout_};
        temporal_pipeline_layout_ = create_pipeline_layout(
            device_, layouts.data(), static_cast<uint32_t>(layouts.size()),
            sizeof(FogPushConstants));
        temporal_pipeline_ = create_compute_pipeline(
            device_, temporal_pipeline_layout_, "shaders/fog_temporal.comp.spv", vk_pipeline_cache_);
    }

    // ── Integrate set layout: binding 0 = sampler3D (scatter), binding 1 = storage image (integrated) ──
    {
        std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ci.bindingCount = static_cast<uint32_t>(bindings.size());
        ci.pBindings = bindings.data();
        vkCreateDescriptorSetLayout(device_, &ci, nullptr, &integrate_set_layout_);

        integrate_pipeline_layout_ = create_pipeline_layout(
            device_, &integrate_set_layout_, 1, sizeof(FogPushConstants));
        integrate_pipeline_ = create_compute_pipeline(
            device_, integrate_pipeline_layout_, "shaders/fog_integrate.comp.spv", vk_pipeline_cache_);
    }

    // ── Composite set layout: binding 0 = sampler3D, binding 1 = sampler2D (depth), binding 2 = storage image (color) ──
    {
        std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        bindings[2].binding = 2;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ci.bindingCount = static_cast<uint32_t>(bindings.size());
        ci.pBindings = bindings.data();
        vkCreateDescriptorSetLayout(device_, &ci, nullptr, &composite_set_layout_);

        std::array layouts{global_set_layout, composite_set_layout_};
        composite_pipeline_layout_ = create_pipeline_layout(
            device_, layouts.data(), static_cast<uint32_t>(layouts.size()),
            sizeof(FogPushConstants));
        composite_pipeline_ = create_compute_pipeline(
            device_, composite_pipeline_layout_, "shaders/fog_composite.comp.spv", vk_pipeline_cache_);
    }

    // Create default images with initial 1x1x1 volumes so descriptors are valid
    default_images_.create(device_, allocator_, 1, 1);

    FJELL_GFX_DEBUG("Volumetric fog pass created");
    return {};
}

void VolumetricFogPass::destroy() {
    if (!device_) return;

    default_images_.destroy(device_, allocator_);

    auto destroy_pass = [&](VkPipeline& p, VkPipelineLayout& l, VkDescriptorSetLayout& sl) {
        if (p) { vkDestroyPipeline(device_, p, nullptr); p = VK_NULL_HANDLE; }
        if (l) { vkDestroyPipelineLayout(device_, l, nullptr); l = VK_NULL_HANDLE; }
        if (sl) { vkDestroyDescriptorSetLayout(device_, sl, nullptr); sl = VK_NULL_HANDLE; }
    };

    destroy_pass(inject_pipeline_, inject_pipeline_layout_, inject_set_layout_);
    destroy_pass(temporal_pipeline_, temporal_pipeline_layout_, temporal_set_layout_);
    destroy_pass(integrate_pipeline_, integrate_pipeline_layout_, integrate_set_layout_);
    destroy_pass(composite_pipeline_, composite_pipeline_layout_, composite_set_layout_);

    if (linear_sampler_) { vkDestroySampler(device_, linear_sampler_, nullptr); linear_sampler_ = VK_NULL_HANDLE; }

    device_ = VK_NULL_HANDLE;
}

void VolumetricFogPass::resize(uint32_t width, uint32_t height) {
    default_images_.resize(device_, allocator_, width, height);
}

VkDescriptorSet VolumetricFogPass::imgui_texture() {
    return default_images_.imgui_texture(linear_sampler_);
}

void VolumetricFogPass::invalidate_imgui_texture() {
    default_images_.invalidate_imgui_texture();
}

// ── Record ──────────────────────────────────────────────────────────────────

static void batch_barriers(VkCommandBuffer cmd,
                            const VkImageMemoryBarrier2* barriers, uint32_t count) {
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = count;
    dep.pImageMemoryBarriers = barriers;
    vkCmdPipelineBarrier2(cmd, &dep);
}

static VkImageMemoryBarrier2 make_barrier(VkImage image,
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

void VolumetricFogPass::record(const FrameContext& ctx) {
    auto* imgs = ctx.pp_resources ? ctx.pp_resources->volumetric_fog() : nullptr;
    record_into(ctx, imgs ? *imgs : default_images_);
}

void VolumetricFogPass::record_into(const FrameContext& ctx, Images& imgs) {
    if (!enabled_) return;
    if (imgs.scatter_volumes[0].view == VK_NULL_HANDLE) return;
    if (ctx.registry == nullptr) return;

    const auto* integrated_res = ctx.registry->find_image("fog_integrated");
    if (integrated_res == nullptr || integrated_res->image == VK_NULL_HANDLE) return;

    const VkImage integrated_image = integrated_res->image;
    const VkImageView integrated_view = integrated_res->view;

    auto cmd = ctx.cmd;
    auto extent = ctx.target->extent();
    uint32_t width = extent.width;
    uint32_t height = extent.height;
    VkDescriptorSet global_set = ctx.global_descriptor_set;
    auto* cache = ctx.descriptor_cache;

    // Ping-pong: current frame writes to [cur], reads history from [prev]
    uint32_t cur = imgs.ping_pong;
    uint32_t prev = 1 - cur;

    // ── Acquire descriptor sets for this frame ─────────────────────────
    // inject: binding 0 = storage scatter[cur]
    FrameCacheBinding inj_b{};
    inj_b.binding = 0;
    inj_b.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    inj_b.image.imageView = imgs.scatter_volumes[cur].view;
    inj_b.image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorSet inject_set = cache->acquire(inject_set_layout_, std::span{&inj_b, 1});

    // temporal: binding 0 = storage scatter[cur], binding 1 = sampled scatter[prev]
    std::array<FrameCacheBinding, 2> temp_b{};
    temp_b[0].binding = 0;
    temp_b[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    temp_b[0].image.imageView = imgs.scatter_volumes[cur].view;
    temp_b[0].image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    temp_b[1].binding = 1;
    temp_b[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    temp_b[1].image.sampler = linear_sampler_;
    temp_b[1].image.imageView = imgs.scatter_volumes[prev].view;
    temp_b[1].image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorSet temporal_set = cache->acquire(temporal_set_layout_, temp_b);

    // integrate: binding 0 = sampled scatter[cur], binding 1 = storage integrated
    std::array<FrameCacheBinding, 2> int_b{};
    int_b[0].binding = 0;
    int_b[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    int_b[0].image.sampler = linear_sampler_;
    int_b[0].image.imageView = imgs.scatter_volumes[cur].view;
    int_b[0].image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    int_b[1].binding = 1;
    int_b[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    int_b[1].image.imageView = integrated_view;
    int_b[1].image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorSet integrate_set = cache->acquire(integrate_set_layout_, int_b);

    // composite: binding 0 = sampled integrated, 1 = sampled depth, 2 = storage color
    std::array<FrameCacheBinding, 3> comp_b{};
    comp_b[0].binding = 0;
    comp_b[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    comp_b[0].image.sampler = linear_sampler_;
    comp_b[0].image.imageView = integrated_view;
    comp_b[0].image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    comp_b[1].binding = 1;
    comp_b[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    comp_b[1].image.sampler = linear_sampler_;
    comp_b[1].image.imageView = ctx.target->depth_view();
    comp_b[1].image.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    comp_b[2].binding = 2;
    comp_b[2].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    comp_b[2].image.imageView = ctx.target->color_view();
    comp_b[2].image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorSet composite_set = cache->acquire(composite_set_layout_, comp_b);

    uint32_t vol_w = imgs.volume_extent.width;
    uint32_t vol_h = imgs.volume_extent.height;
    uint32_t vol_d = imgs.volume_extent.depth;

    // Precompute Halton jitter on CPU
    int frame_idx = static_cast<int>(imgs.frame_counter % 16) + 1;

    FogPushConstants push{};
    push.volume_dims = glm::uvec4(vol_w, vol_h, vol_d, imgs.frame_counter);
    push.near_plane = 0.1f;
    push.far_plane = max_distance_;
    push.density = density_;
    push.height = height_;
    push.height_falloff = height_falloff_;
    push.scattering = scattering_;
    push.anisotropy = anisotropy_;
    push.temporal_blend = temporal_blend_;
    push.viewport_width = width;
    push.viewport_height = height;
    push.fog_color_r = fog_color_.r;
    push.fog_color_g = fog_color_.g;
    push.fog_color_b = fog_color_.b;
    push.jitter_x = halton(frame_idx, 3) - 0.5f;
    push.jitter_y = halton(frame_idx, 5) - 0.5f;
    push.jitter_z = halton(frame_idx, 2) - 0.5f;

    uint32_t groups_x = (vol_w + TILE_SIZE - 1) / TILE_SIZE;
    uint32_t groups_y = (vol_h + TILE_SIZE - 1) / TILE_SIZE;

    // ── 1. Initialize volumes on first frame / after resize ──────────
    if (imgs.volumes_need_init) {
        std::array<VkImageMemoryBarrier2, 3> init{};
        init[0] = make_barrier(imgs.scatter_volumes[0].image,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        init[1] = make_barrier(imgs.scatter_volumes[1].image,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        init[2] = make_barrier(integrated_image,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        batch_barriers(cmd, init.data(), static_cast<uint32_t>(init.size()));

        imgs.volumes_need_init = false;
    }

    // ── 2. Inject: density + light scattering → scatter_volumes[cur] ──
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, inject_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, inject_pipeline_layout_,
                            0, 1, &global_set, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, inject_pipeline_layout_,
                            1, 1, &inject_set, 0, nullptr);
    vkCmdPushConstants(cmd, inject_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);
    vkCmdDispatch(cmd, groups_x, groups_y, vol_d);

    // ── 3. Barrier: inject write → temporal read-write ───────────────
    {
        auto b = make_barrier(imgs.scatter_volumes[cur].image,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        batch_barriers(cmd, &b, 1);
    }

    // ── 4. Temporal: blend scatter_volumes[cur] with scatter_volumes[prev] ──
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, temporal_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, temporal_pipeline_layout_,
                            0, 1, &global_set, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, temporal_pipeline_layout_,
                            1, 1, &temporal_set, 0, nullptr);
    vkCmdPushConstants(cmd, temporal_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);
    vkCmdDispatch(cmd, groups_x, groups_y, vol_d);

    // ── 5. Barriers: scatter[cur] → readable for integrate, scatter[prev] stays readable ──
    {
        auto b = make_barrier(imgs.scatter_volumes[cur].image,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        batch_barriers(cmd, &b, 1);
    }

    // ── 6. Integrate: front-to-back ray march ────────────────────────
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, integrate_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, integrate_pipeline_layout_,
                            0, 1, &integrate_set, 0, nullptr);
    vkCmdPushConstants(cmd, integrate_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);
    vkCmdDispatch(cmd, groups_x, groups_y, 1);

    // ── 7. Barrier: integrated → readable for composite ──────────────
    {
        auto b = make_barrier(integrated_image,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        batch_barriers(cmd, &b, 1);
    }

    // ── 8. Composite: apply fog to scene color ───────────────────────
    uint32_t screen_groups_x = (width + TILE_SIZE - 1) / TILE_SIZE;
    uint32_t screen_groups_y = (height + TILE_SIZE - 1) / TILE_SIZE;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, composite_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, composite_pipeline_layout_,
                            0, 1, &global_set, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, composite_pipeline_layout_,
                            1, 1, &composite_set, 0, nullptr);
    vkCmdPushConstants(cmd, composite_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);
    vkCmdDispatch(cmd, screen_groups_x, screen_groups_y, 1);

    // ── 9. Restore layouts for next frame ────────────────────────────
    // scatter[cur]: SHADER_READ_ONLY → stays there (becomes prev next frame)
    // integrated: SHADER_READ_ONLY → GENERAL (for next frame's write)
    // scatter[prev]: SHADER_READ_ONLY → GENERAL (becomes cur next frame)
    {
        std::array<VkImageMemoryBarrier2, 2> restore{};
        restore[0] = make_barrier(integrated_image,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        restore[1] = make_barrier(imgs.scatter_volumes[prev].image,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        batch_barriers(cmd, restore.data(), static_cast<uint32_t>(restore.size()));
    }

    // Flip ping-pong for next frame
    imgs.ping_pong = prev;
    ++imgs.frame_counter;
}

void VolumetricFogPass::prepare_frame(const FrameContext& ctx, GpuContext& /*gpu*/) {
    enabled_ = false;
    if (!ctx.scene) return;

    for (const auto& obj : ctx.scene->objects()) {
        if (const auto* fog = dynamic_cast<const VolumetricFogNode*>(obj.get())) {
            enabled_ = true;
            density_ = fog->density;
            height_ = glm::vec3(fog->world_matrix_[3]).y;
            height_falloff_ = fog->height_falloff;
            scattering_ = fog->scattering;
            anisotropy_ = fog->anisotropy;
            max_distance_ = fog->max_distance;
            temporal_blend_ = fog->temporal_blend;
            fog_color_ = fog->fog_color;
            break;
        }
    }
}

PassSetup VolumetricFogPass::setup() const {
    return {
        .name = "volumetric_fog",
        .phase = RenderPhase::PostProcess,
        .sub_order = 100,
    };
}

void VolumetricFogPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    auto depth = builder.import_named(ctx, "depth");
    auto color = builder.import_named(ctx, "color");
    builder.read(depth, ResourceAccess::sampled_compute);
    builder.write(color, ResourceAccess::storage_write_compute);

    // Froxel 3D scatter volumes ping-pong across frames, so they're
    // persistent and sit out of the alias pool. The single-frame
    // integrated volume is a real transient that can alias.
    TextureDesc scatter_desc{};
    scatter_desc.format = VOLUME_FORMAT;
    scatter_desc.size_class = SizeClass::viewport;
    scatter_desc.viewport_divisor = SCREEN_DIVISOR;
    scatter_desc.depth = DEPTH_SLICES;
    scatter_desc.view_type = VK_IMAGE_VIEW_TYPE_3D;
    scatter_desc.persistent = true;
    auto scatter0 = builder.create("fog_scatter0", scatter_desc);
    auto scatter1 = builder.create("fog_scatter1", scatter_desc);
    builder.write(scatter0, ResourceAccess::storage_write_compute);
    builder.write(scatter1, ResourceAccess::storage_write_compute);

    TextureDesc integrated_desc = scatter_desc;
    integrated_desc.persistent = false;
    auto integrated = builder.create("fog_integrated", integrated_desc);
    builder.write(integrated, ResourceAccess::storage_write_compute);
    builder.read(integrated, ResourceAccess::sampled_compute);
}

} // namespace fjell
