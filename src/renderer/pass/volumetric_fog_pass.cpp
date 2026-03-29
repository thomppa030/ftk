#include "renderer/pass/volumetric_fog_pass.hpp"
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

    VkPipeline pipeline;
    if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci,
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
        VkDevice device, VkPhysicalDevice physical_device,
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

    if (vkCreateImage(device, &ci, nullptr, &img.image) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create fog volume image");
    }

    VkMemoryRequirements reqs;
    vkGetImageMemoryRequirements(device, img.image, &reqs);

    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = reqs.size;
    alloc.memoryTypeIndex = Buffer::find_memory_type(
        physical_device, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(device, &alloc, nullptr, &img.memory) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate fog volume memory");
    }
    vkBindImageMemory(device, img.image, img.memory, 0);

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

void VolumetricFogPass::destroy_volume_image(VkDevice device, VolumeImage& img) {
    if (img.view) vkDestroyImageView(device, img.view, nullptr);
    if (img.image) vkDestroyImage(device, img.image, nullptr);
    if (img.memory) vkFreeMemory(device, img.memory, nullptr);
    img = {};
}

// ── Images ──────────────────────────────────────────────────────────────────

void VolumetricFogPass::Images::create(VkDevice device, VkPhysicalDevice physical_device,
                                        VkDescriptorSetLayout inject_layout,
                                        VkDescriptorSetLayout temporal_layout,
                                        VkDescriptorSetLayout integrate_layout,
                                        VkDescriptorSetLayout composite_layout,
                                        VkSampler linear_sampler,
                                        uint32_t vol_w, uint32_t vol_h) {
    volume_extent = {vol_w, vol_h, DEPTH_SLICES};
    volumes_need_init = true;
    ping_pong = 0;
    frame_counter = 0;

    // Create volume images
    scatter_volumes[0] = create_volume_image(device, physical_device, vol_w, vol_h, DEPTH_SLICES);
    scatter_volumes[1] = create_volume_image(device, physical_device, vol_w, vol_h, DEPTH_SLICES);
    integrated_volume = create_volume_image(device, physical_device, vol_w, vol_h, DEPTH_SLICES);

    // Consolidated descriptor pool:
    // Sets: 2 (inject) + 2 (temporal) + 2 (integrate) + MAX_FRAMES_IN_FLIGHT (composite) = 9
    // Storage images: 2 (inject) + 2 (temporal) + 2 (integrate) + MAX_FRAMES_IN_FLIGHT (composite) = 9
    // Combined image samplers: 0 (inject) + 2 (temporal) + 2 (integrate) + 2*MAX_FRAMES_IN_FLIGHT (composite) = 10
    constexpr uint32_t total_sets = 2 + 2 + 2 + MAX_FRAMES_IN_FLIGHT;
    constexpr uint32_t total_storage = 2 + 2 + 2 + MAX_FRAMES_IN_FLIGHT;
    constexpr uint32_t total_sampler = 2 + 2 + 2 * MAX_FRAMES_IN_FLIGHT;

    std::array<VkDescriptorPoolSize, 2> pool_sizes{};
    pool_sizes[0] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, total_storage};
    pool_sizes[1] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, total_sampler};

    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.maxSets = total_sets;
    pool_ci.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
    pool_ci.pPoolSizes = pool_sizes.data();
    vkCreateDescriptorPool(device, &pool_ci, nullptr, &pool);

    // Allocate inject sets
    {
        std::array<VkDescriptorSetLayout, 2> layouts{inject_layout, inject_layout};
        VkDescriptorSetAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool = pool;
        alloc.descriptorSetCount = 2;
        alloc.pSetLayouts = layouts.data();
        vkAllocateDescriptorSets(device, &alloc, inject_sets);
    }

    // Allocate temporal sets
    {
        std::array<VkDescriptorSetLayout, 2> layouts{temporal_layout, temporal_layout};
        VkDescriptorSetAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool = pool;
        alloc.descriptorSetCount = 2;
        alloc.pSetLayouts = layouts.data();
        vkAllocateDescriptorSets(device, &alloc, temporal_sets);
    }

    // Allocate integrate sets
    {
        std::array<VkDescriptorSetLayout, 2> layouts{integrate_layout, integrate_layout};
        VkDescriptorSetAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool = pool;
        alloc.descriptorSetCount = 2;
        alloc.pSetLayouts = layouts.data();
        vkAllocateDescriptorSets(device, &alloc, integrate_sets);
    }

    // Allocate composite sets
    {
        std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> layouts{};
        layouts.fill(composite_layout);
        VkDescriptorSetAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool = pool;
        alloc.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
        alloc.pSetLayouts = layouts.data();
        vkAllocateDescriptorSets(device, &alloc, composite_sets);
    }

    // Update inject descriptors (binding 0 = storage image for scatter write)
    for (uint32_t i = 0; i < 2; ++i) {
        VkDescriptorImageInfo storage_info{};
        storage_info.imageView = scatter_volumes[i].view;
        storage_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = inject_sets[i];
        write.dstBinding = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        write.descriptorCount = 1;
        write.pImageInfo = &storage_info;

        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }

    // Update temporal descriptors (binding 0 = storage rw current, binding 1 = sampler prev)
    for (uint32_t i = 0; i < 2; ++i) {
        uint32_t other = 1 - i;

        VkDescriptorImageInfo storage_info{};
        storage_info.imageView = scatter_volumes[i].view;
        storage_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo sampled_info{};
        sampled_info.sampler = linear_sampler;
        sampled_info.imageView = scatter_volumes[other].view;
        sampled_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        std::array<VkWriteDescriptorSet, 2> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = temporal_sets[i];
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].descriptorCount = 1;
        writes[0].pImageInfo = &storage_info;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = temporal_sets[i];
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo = &sampled_info;

        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }

    // Update integrate descriptors (binding 0 = sampler scatter, binding 1 = storage integrated)
    for (uint32_t i = 0; i < 2; ++i) {
        VkDescriptorImageInfo sampled_info{};
        sampled_info.sampler = linear_sampler;
        sampled_info.imageView = scatter_volumes[i].view;
        sampled_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo storage_info{};
        storage_info.imageView = integrated_volume.view;
        storage_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        std::array<VkWriteDescriptorSet, 2> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = integrate_sets[i];
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].descriptorCount = 1;
        writes[0].pImageInfo = &sampled_info;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = integrate_sets[i];
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo = &storage_info;

        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
}

void VolumetricFogPass::Images::destroy(VkDevice device) {
    invalidate_imgui_texture();
    if (pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, pool, nullptr);
        pool = VK_NULL_HANDLE;
        for (auto& s : inject_sets) s = VK_NULL_HANDLE;
        for (auto& s : temporal_sets) s = VK_NULL_HANDLE;
        for (auto& s : integrate_sets) s = VK_NULL_HANDLE;
        for (auto& s : composite_sets) s = VK_NULL_HANDLE;
    }
    destroy_volume_image(device, scatter_volumes[0]);
    destroy_volume_image(device, scatter_volumes[1]);
    destroy_volume_image(device, integrated_volume);
    volume_extent = {};
}

void VolumetricFogPass::Images::resize(VkDevice device, VkPhysicalDevice physical_device,
                                        VkDescriptorSetLayout inject_layout,
                                        VkDescriptorSetLayout temporal_layout,
                                        VkDescriptorSetLayout integrate_layout,
                                        VkDescriptorSetLayout composite_layout,
                                        VkSampler linear_sampler,
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

    destroy(device);
    create(device, physical_device, inject_layout, temporal_layout, integrate_layout,
           composite_layout, linear_sampler, vol_w, vol_h);
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

// ── Composite descriptor update (per-frame, called from record_into) ────────

static void update_composite_descriptors(VkDevice device, VkSampler linear_sampler,
                                          VkDescriptorSet set,
                                          VkImageView integrated_view,
                                          VkImageView depth_view,
                                          VkImageView color_view) {
    VkDescriptorImageInfo fog_info{};
    fog_info.sampler = linear_sampler;
    fog_info.imageView = integrated_view;
    fog_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo depth_info{};
    depth_info.sampler = linear_sampler;
    depth_info.imageView = depth_view;
    depth_info.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo color_info{};
    color_info.imageView = color_view;
    color_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    std::array<VkWriteDescriptorSet, 3> writes{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo = &fog_info;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = set;
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo = &depth_info;

    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = set;
    writes[2].dstBinding = 2;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[2].descriptorCount = 1;
    writes[2].pImageInfo = &color_info;

    vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

// ── VolumetricFogPass (singleton) ───────────────────────────────────────────

void VolumetricFogPass::create(Device& device, const PassResources& resources) {
    device_ = device.handle();
    physical_device_ = device.physical_device();
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
        throw std::runtime_error("Failed to create fog sampler");
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
            device_, inject_pipeline_layout_, "shaders/fog_inject.comp.spv");
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
            device_, temporal_pipeline_layout_, "shaders/fog_temporal.comp.spv");
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
            device_, integrate_pipeline_layout_, "shaders/fog_integrate.comp.spv");
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
            device_, composite_pipeline_layout_, "shaders/fog_composite.comp.spv");
    }

    // Create default images with initial 1x1x1 volumes so descriptors are valid
    default_images_.create(device_, physical_device_,
                           inject_set_layout_, temporal_set_layout_,
                           integrate_set_layout_, composite_set_layout_,
                           linear_sampler_, 1, 1);

    FJELL_GFX_DEBUG("Volumetric fog pass created");
}

void VolumetricFogPass::destroy() {
    if (!device_) return;

    default_images_.destroy(device_);

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
    default_images_.resize(device_, physical_device_,
                           inject_set_layout_, temporal_set_layout_,
                           integrate_set_layout_, composite_set_layout_,
                           linear_sampler_, width, height);
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
    record_into(ctx, default_images_);
}

void VolumetricFogPass::record_into(const FrameContext& ctx, Images& imgs) {
    if (!enabled_) return;
    if (imgs.scatter_volumes[0].view == VK_NULL_HANDLE) return;

    auto cmd = ctx.cmd;
    auto extent = ctx.target->extent();
    uint32_t width = extent.width;
    uint32_t height = extent.height;
    uint32_t frame_index = ctx.frame_index;
    VkDescriptorSet global_set = ctx.global_descriptor_set;

    // Ping-pong: current frame writes to [cur], reads history from [prev]
    uint32_t cur = imgs.ping_pong;
    uint32_t prev = 1 - cur;

    // Always update this frame's composite set (each frame has its own, safe to update)
    update_composite_descriptors(device_, linear_sampler_,
                                 imgs.composite_sets[frame_index],
                                 imgs.integrated_volume.view,
                                 ctx.target->depth_view(),
                                 ctx.target->color_view());

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
        init[2] = make_barrier(imgs.integrated_volume.image,
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
                            1, 1, &imgs.inject_sets[cur], 0, nullptr);
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
                            1, 1, &imgs.temporal_sets[cur], 0, nullptr);
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
                            0, 1, &imgs.integrate_sets[cur], 0, nullptr);
    vkCmdPushConstants(cmd, integrate_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);
    vkCmdDispatch(cmd, groups_x, groups_y, 1);

    // ── 7. Barrier: integrated → readable for composite ──────────────
    {
        auto b = make_barrier(imgs.integrated_volume.image,
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
                            1, 1, &imgs.composite_sets[frame_index], 0, nullptr);
    vkCmdPushConstants(cmd, composite_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);
    vkCmdDispatch(cmd, screen_groups_x, screen_groups_y, 1);

    // ── 9. Restore layouts for next frame ────────────────────────────
    // scatter[cur]: SHADER_READ_ONLY → stays there (becomes prev next frame)
    // integrated: SHADER_READ_ONLY → GENERAL (for next frame's write)
    // scatter[prev]: SHADER_READ_ONLY → GENERAL (becomes cur next frame)
    {
        std::array<VkImageMemoryBarrier2, 2> restore{};
        restore[0] = make_barrier(imgs.integrated_volume.image,
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

} // namespace fjell
