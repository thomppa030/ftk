#include "renderer/pass/ddgi_pass.hpp"
#include "core/profiler.hpp"
#include "renderer/pass/pass_gpu_zone.hpp"
#include "renderer/gpu/accel_structure.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/post_process_resources.hpp"
#include "renderer/resource_registry.hpp"
#include "renderer/render_queue.hpp"
#include "renderer/systems/particle_system.hpp"
#include "renderer/gpu/vma_image.hpp"
#include "renderer/indirect_batch.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/resources/shader_utils.hpp"
#include "renderer/pass/scene_sdf_pass.hpp"
#include "scene/light_node.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#include <imgui_impl_vulkan.h>

#include <glm/gtc/random.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <random>

namespace fjell {

// ── Images ──────────────────────────────────────────────────────────────

static void compute_atlas_dims(const DdgiConfig& config,
                                int32_t& irr_w, int32_t& irr_h,
                                int32_t& vis_w, int32_t& vis_h,
                                int32_t& total) {
    // Every clipmap level draws from one shared probe pool, so the atlases
    // are sized for all levels at once.
    int levels = glm::clamp(config.clipmap_levels, 1, DDGI_MAX_CLIPMAP_LEVELS);
    total = config.probe_counts.x * config.probe_counts.y * config.probe_counts.z * levels;
    int probes_per_row = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(total))));

    int irr_side_with_border = config.irradiance_side_length + 2;
    irr_w = probes_per_row * irr_side_with_border;
    irr_h = ((total + probes_per_row - 1) / probes_per_row) * irr_side_with_border;

    int vis_side_with_border = config.visibility_side_length + 2;
    vis_w = probes_per_row * vis_side_with_border;
    vis_h = ((total + probes_per_row - 1) / probes_per_row) * vis_side_with_border;
}

void DdgiPass::Images::create(VkDevice device, VmaAllocator allocator,
                               const DdgiConfig& config,
                               uint32_t screen_w, uint32_t screen_h) {
    compute_atlas_dims(config, irr_width, irr_height, vis_width, vis_height, total_probes);

    auto create_image = [&](VkImage& img, VmaAllocation& alloc, VkImageView& view,
                            uint32_t w, uint32_t h, VkFormat format,
                            VkImageUsageFlags usage) {
        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {w, h, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        create_vma_image(allocator, &info, &img, &alloc);

        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = img;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = format;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        vkCreateImageView(device, &view_info, nullptr, &view);
    };

    VkImageUsageFlags storage_sampled = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    // Radiance: probe_rays x total_probes
    create_image(radiance_image, radiance_memory, radiance_view,
                 static_cast<uint32_t>(config.probe_rays),
                 static_cast<uint32_t>(total_probes),
                 VK_FORMAT_R16G16B16A16_SFLOAT, storage_sampled);

    // Irradiance atlas
    create_image(irradiance_image, irradiance_memory, irradiance_view,
                 static_cast<uint32_t>(irr_width),
                 static_cast<uint32_t>(irr_height),
                 VK_FORMAT_R16G16B16A16_SFLOAT, storage_sampled);

    // Visibility atlas
    create_image(visibility_image, visibility_memory, visibility_view,
                 static_cast<uint32_t>(vis_width),
                 static_cast<uint32_t>(vis_height),
                 VK_FORMAT_R16G16_SFLOAT, storage_sampled);

    // Probe offsets: one texel per probe, addressed as
    // (index % probes_xy, index / probes_xy) — so the row count has to
    // cover the whole pool across every clipmap level, not one level's
    // probe_counts.z.
    int probes_xy = config.probe_counts.x * config.probe_counts.y;
    int offset_rows = (total_probes + probes_xy - 1) / probes_xy;
    create_image(probe_offset_image, probe_offset_memory, probe_offset_view,
                 static_cast<uint32_t>(probes_xy),
                 static_cast<uint32_t>(offset_rows),
                 VK_FORMAT_R16G16B16A16_SFLOAT, storage_sampled);

    // Indirection: grid coordinate -> probe slot, one texel per cell of the
    // clipmap level. Double-buffered so the reallocation pass can read the
    // previous mapping while writing the new one without a hazard on a
    // single image. R32_UINT rather than R16_UINT: the current probe count
    // fits in 16 bits, but the spare high bits are where per-cell flags go
    // once levels are stacked, and the whole pair is only a few KB.
    auto create_indirection = [&](VkImage& img, VmaAllocation& alloc, VkImageView& view) {
        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_3D;
        info.format = VK_FORMAT_R32_UINT;
        // Levels are packed side by side along X, matching Lumen's
        // `ProbeCoord.x + ClipmapIndex * Resolution`.
        int level_count = glm::clamp(config.clipmap_levels, 1, DDGI_MAX_CLIPMAP_LEVELS);
        info.extent = {static_cast<uint32_t>(config.probe_counts.x * level_count),
                       static_cast<uint32_t>(config.probe_counts.y),
                       static_cast<uint32_t>(config.probe_counts.z)};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        create_vma_image(allocator, &info, &img, &alloc);

        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = img;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_3D;
        view_info.format = VK_FORMAT_R32_UINT;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        vkCreateImageView(device, &view_info, nullptr, &view);
    };

    for (int i = 0; i < 2; ++i) {
        create_indirection(indirection_image[i], indirection_memory[i], indirection_view[i]);
    }
    indirection_current = 0;

    // Slot -> grid coordinate, the inverse of the indirection texture. The
    // trace/update dispatches run one invocation per slot and need the world
    // position that slot currently stands for; deriving it arithmetically
    // from the slot index (as the frozen grid did) is exactly what stops
    // working once slots are reused for different cells.
    VkDeviceSize slot_grid_size = static_cast<VkDeviceSize>(total_probes) * sizeof(uint32_t) * 4;
    slot_grid_buffer = std::make_unique<Buffer>(
        allocator, slot_grid_size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO);

    // Free list for the reallocation pass: a leading counter followed by
    // one slot per probe. Worst case (a shift clearing the whole volume)
    // every slot is freed at once, so it never needs to hold more.
    // Two leading counters (free_count, high_water) then one slot per probe.
    VkDeviceSize free_list_size = sizeof(uint32_t) * (2 + static_cast<VkDeviceSize>(total_probes));
    free_list_buffer = std::make_unique<Buffer>(
        allocator, free_list_size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO);

    // Frame each slot was last asked for, so a probe that leaves view
    // briefly is kept rather than freed and retraced when it returns.
    VkDeviceSize last_used_size = static_cast<VkDeviceSize>(total_probes) * sizeof(uint32_t);
    probe_last_used_buffer = std::make_unique<Buffer>(
        allocator, last_used_size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO);

    // Probe status SSBO
    VkDeviceSize status_size = static_cast<VkDeviceSize>(total_probes) * sizeof(uint32_t);
    probe_status_buffer = std::make_unique<Buffer>(
        allocator, status_size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO);

    // Constants UBO
    constants_ubo = std::make_unique<Buffer>(
        allocator, sizeof(GpuDdgiConstants),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    constants_ubo->map();

    // Indirect output (screen-res or half-res)
    uint32_t out_w = config.half_res_output ? std::max(1u, screen_w / 2) : screen_w;
    uint32_t out_h = config.half_res_output ? std::max(1u, screen_h / 2) : screen_h;
    indirect_extent = {out_w, out_h};

    create_image(indirect_image, indirect_memory, indirect_view,
                 out_w, out_h,
                 VK_FORMAT_R16G16B16A16_SFLOAT, storage_sampled);

    images_need_init = true;
}

void DdgiPass::Images::destroy(VkDevice device, VmaAllocator allocator) {
    auto destroy_img = [&](VkImageView& view, VkImage& img, VmaAllocation& alloc) {
        if (view != VK_NULL_HANDLE) vkDestroyImageView(device, view, nullptr);
        destroy_vma_image(allocator, img, alloc);
        view = VK_NULL_HANDLE;
        img = VK_NULL_HANDLE;
        alloc = VK_NULL_HANDLE;
    };

    if (desc_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, desc_pool, nullptr);
        desc_pool = VK_NULL_HANDLE;
        trace_set = VK_NULL_HANDLE;
    }

    destroy_img(radiance_view, radiance_image, radiance_memory);
    destroy_img(irradiance_view, irradiance_image, irradiance_memory);
    destroy_img(visibility_view, visibility_image, visibility_memory);
    destroy_img(probe_offset_view, probe_offset_image, probe_offset_memory);
    destroy_img(indirect_view, indirect_image, indirect_memory);
    for (int i = 0; i < 2; ++i) {
        destroy_img(indirection_view[i], indirection_image[i], indirection_memory[i]);
    }

    slot_grid_buffer.reset();
    free_list_buffer.reset();
    probe_last_used_buffer.reset();
    probe_status_buffer.reset();
    constants_ubo.reset();
}

void DdgiPass::Images::resize(VkDevice device, VmaAllocator allocator,
                               const DdgiConfig& config,
                               uint32_t screen_w, uint32_t screen_h) {
    // First call: create everything
    if (radiance_image == VK_NULL_HANDLE) {
        create(device, allocator, config, screen_w, screen_h);
        return;
    }

    // Subsequent calls: only recreate indirect_image if screen size changed
    uint32_t new_w = config.half_res_output ? std::max(1u, screen_w / 2) : screen_w;
    uint32_t new_h = config.half_res_output ? std::max(1u, screen_h / 2) : screen_h;
    if (indirect_extent.width == new_w && indirect_extent.height == new_h) return;

    // Only destroy/recreate the indirect output image, not the probe images
    if (indirect_view != VK_NULL_HANDLE) vkDestroyImageView(device, indirect_view, nullptr);
    destroy_vma_image(allocator, indirect_image, indirect_memory);
    indirect_view = VK_NULL_HANDLE;
    indirect_image = VK_NULL_HANDLE;
    indirect_memory = VK_NULL_HANDLE;

    indirect_extent = {new_w, new_h};

    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    info.extent = {new_w, new_h, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    create_vma_image(allocator, &info, &indirect_image, &indirect_memory);

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = indirect_image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    vkCreateImageView(device, &view_info, nullptr, &indirect_view);

    // Mark descriptors as needing rewrite (sample set references indirect_view)
    images_need_init = true;
}

void DdgiPass::Images::invalidate_imgui_texture() {
    if (indirect_imgui_desc != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(indirect_imgui_desc);
        indirect_imgui_desc = VK_NULL_HANDLE;
    }
    if (irradiance_imgui_desc != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(irradiance_imgui_desc);
        irradiance_imgui_desc = VK_NULL_HANDLE;
    }
}

VkDescriptorSet DdgiPass::Images::indirect_imgui_texture(VkSampler sampler) {
    if (indirect_view == VK_NULL_HANDLE || images_need_init) return VK_NULL_HANDLE;
    if (indirect_imgui_desc == VK_NULL_HANDLE) {
        indirect_imgui_desc = ImGui_ImplVulkan_AddTexture(
            sampler, indirect_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return indirect_imgui_desc;
}

VkDescriptorSet DdgiPass::Images::irradiance_imgui_texture(VkSampler sampler) {
    if (irradiance_view == VK_NULL_HANDLE || images_need_init) return VK_NULL_HANDLE;
    if (irradiance_imgui_desc == VK_NULL_HANDLE) {
        irradiance_imgui_desc = ImGui_ImplVulkan_AddTexture(
            sampler, irradiance_view, VK_IMAGE_LAYOUT_GENERAL);
    }
    return irradiance_imgui_desc;
}

// ── DdgiPass ────────────────────────────────────────────────────────────

Result<> DdgiPass::create(Device& device, const PassResources& resources) {
    device_ = &device;
    vk_device_ = device.handle();

    // Linear sampler for SDF and atlas textures
    VkSamplerCreateInfo sampler_ci{};
    sampler_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_ci.magFilter = VK_FILTER_LINEAR;
    sampler_ci.minFilter = VK_FILTER_LINEAR;
    sampler_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

    if (vkCreateSampler(vk_device_, &sampler_ci, nullptr, &linear_sampler_) != VK_SUCCESS) {
        return make_error("Failed to create DDGI sampler");
    }

    // Descriptor set layout for SDF trace:
    // binding 0: DDGIConstants UBO
    // binding 1: scene SDF (sampler3D)
    // binding 2: radiance output (storage image)
    // binding 3: sky cubemap (samplerCube)
    // binding 4: probe status SSBO
    // binding 5: probe offset texture (sampler2D)
    std::array<VkDescriptorSetLayoutBinding, 7> bindings{};

    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
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

    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[4].binding = 4;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[5].binding = 5;
    bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[5].descriptorCount = 1;
    bindings[5].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    // 6: slot -> grid coord, so the trace knows where each slot currently sits
    bindings[6].binding = 6;
    bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[6].descriptorCount = 1;
    bindings[6].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_ci.bindingCount = static_cast<uint32_t>(bindings.size());
    layout_ci.pBindings = bindings.data();

    if (vkCreateDescriptorSetLayout(vk_device_, &layout_ci, nullptr, &trace_set_layout_) != VK_SUCCESS) {
        return make_error("Failed to create DDGI trace set layout");
    }

    // Pipeline layout: descriptor set + push constants
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(TracePushConstants);

    VkPipelineLayoutCreateInfo pipe_layout_ci{};
    pipe_layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipe_layout_ci.setLayoutCount = 1;
    pipe_layout_ci.pSetLayouts = &trace_set_layout_;
    pipe_layout_ci.pushConstantRangeCount = 1;
    pipe_layout_ci.pPushConstantRanges = &push_range;

    if (vkCreatePipelineLayout(vk_device_, &pipe_layout_ci, nullptr, &trace_pipeline_layout_) != VK_SUCCESS) {
        return make_error("Failed to create DDGI trace pipeline layout");
    }

    // Compute pipeline
    auto spirv = read_spirv(base_path::resolve("shaders/ddgi_probe_trace_sdf.comp.spv"));
    VkShaderModule module = create_shader_module(vk_device_, spirv);

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module;
    stage.pName = "main";

    VkComputePipelineCreateInfo pipe_ci{};
    pipe_ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipe_ci.stage = stage;
    pipe_ci.layout = trace_pipeline_layout_;

    VkResult result = vkCreateComputePipelines(vk_device_, resources.pipeline_cache, 1, &pipe_ci, nullptr, &trace_pipeline_);
    vkDestroyShaderModule(vk_device_, module, nullptr);

    if (result != VK_SUCCESS) {
        return make_error("Failed to create DDGI SDF trace pipeline");
    }

    // ── Update descriptor set layout ──
    // binding 0: DDGIConstants UBO
    // binding 1: radiance texture (sampler2D)
    // binding 2: irradiance image (storage, rw)
    // binding 3: visibility image (storage, rw)
    // binding 4: probe offset image (storage, rw)
    // binding 5: probe status SSBO (rw)
    std::array<VkDescriptorSetLayoutBinding, 6> update_bindings{};
    update_bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    update_bindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    update_bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    update_bindings[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    update_bindings[4] = {4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    update_bindings[5] = {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

    VkDescriptorSetLayoutCreateInfo update_layout_ci{};
    update_layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    update_layout_ci.bindingCount = static_cast<uint32_t>(update_bindings.size());
    update_layout_ci.pBindings = update_bindings.data();

    if (vkCreateDescriptorSetLayout(vk_device_, &update_layout_ci, nullptr, &update_set_layout_) != VK_SUCCESS) {
        return make_error("Failed to create DDGI update set layout");
    }

    // Update pipeline layout: descriptor set + push constants (first_frame + max_trace_dist)
    VkPushConstantRange update_push{};
    update_push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    update_push.offset = 0;
    update_push.size = sizeof(uint32_t) + sizeof(float);

    VkPipelineLayoutCreateInfo update_pipe_layout_ci{};
    update_pipe_layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    update_pipe_layout_ci.setLayoutCount = 1;
    update_pipe_layout_ci.pSetLayouts = &update_set_layout_;
    update_pipe_layout_ci.pushConstantRangeCount = 1;
    update_pipe_layout_ci.pPushConstantRanges = &update_push;

    if (vkCreatePipelineLayout(vk_device_, &update_pipe_layout_ci, nullptr, &update_pipeline_layout_) != VK_SUCCESS) {
        return make_error("Failed to create DDGI update pipeline layout");
    }

    // Helper: create compute pipeline from SPIR-V path
    auto create_compute = [&](const std::string& spv_path, VkPipeline& out_pipeline) -> VkResult {
        auto code = read_spirv(base_path::resolve(spv_path));
        VkShaderModule mod = create_shader_module(vk_device_, code);
        VkPipelineShaderStageCreateInfo s{};
        s.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        s.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        s.module = mod;
        s.pName = "main";
        VkComputePipelineCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ci.stage = s;
        ci.layout = update_pipeline_layout_;
        VkResult r = vkCreateComputePipelines(vk_device_, resources.pipeline_cache, 1, &ci, nullptr, &out_pipeline);
        vkDestroyShaderModule(vk_device_, mod, nullptr);
        return r;
    };

    if (create_compute("shaders/ddgi_irradiance_update.comp.spv", irradiance_update_pipeline_) != VK_SUCCESS)
        return make_error("Failed to create DDGI irradiance update pipeline");
    if (create_compute("shaders/ddgi_visibility_update.comp.spv", visibility_update_pipeline_) != VK_SUCCESS)
        return make_error("Failed to create DDGI visibility update pipeline");
    if (create_compute("shaders/ddgi_probe_offsets.comp.spv", probe_offset_pipeline_) != VK_SUCCESS)
        return make_error("Failed to create DDGI probe offset pipeline");
    if (create_compute("shaders/ddgi_probe_status.comp.spv", probe_status_pipeline_) != VK_SUCCESS)
        return make_error("Failed to create DDGI probe status pipeline");

    // ── Clipmap reallocation descriptor set layout ──
    // binding 0: DDGIConstants UBO
    // binding 1: previous indirection image (storage 3D, read)
    // binding 2: next indirection image (storage 3D, write)
    // binding 3: slot -> grid coord SSBO (rw)
    // binding 4: probe status SSBO (rw)
    // binding 5: free list SSBO (rw)
    // binding 6: probe last-used-frame SSBO (rw)
    std::array<VkDescriptorSetLayoutBinding, 7> alloc_bindings{};
    alloc_bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    alloc_bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    alloc_bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    alloc_bindings[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    alloc_bindings[4] = {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    alloc_bindings[5] = {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    alloc_bindings[6] = {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

    // Every binding here is rewritten on each shift — the two indirection
    // images swap roles, and the rest are re-pointed alongside them. A shift
    // is driven by camera movement, so it lands mid-flight whenever an
    // earlier frame's command buffer still references this set.
    std::array<VkDescriptorBindingFlags, 7> alloc_binding_flags{};
    for (auto& f : alloc_binding_flags) {
        f = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    }

    VkDescriptorSetLayoutBindingFlagsCreateInfo alloc_flags_ci{};
    alloc_flags_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    alloc_flags_ci.bindingCount = static_cast<uint32_t>(alloc_binding_flags.size());
    alloc_flags_ci.pBindingFlags = alloc_binding_flags.data();

    VkDescriptorSetLayoutCreateInfo alloc_layout_ci{};
    alloc_layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    alloc_layout_ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    alloc_layout_ci.pNext = &alloc_flags_ci;
    alloc_layout_ci.bindingCount = static_cast<uint32_t>(alloc_bindings.size());
    alloc_layout_ci.pBindings = alloc_bindings.data();

    if (vkCreateDescriptorSetLayout(vk_device_, &alloc_layout_ci, nullptr, &alloc_set_layout_) != VK_SUCCESS) {
        return make_error("Failed to create DDGI clipmap alloc set layout");
    }

    VkPushConstantRange alloc_push{};
    alloc_push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    alloc_push.offset = 0;
    alloc_push.size = sizeof(ClipmapAllocPushConstants);

    VkPipelineLayoutCreateInfo alloc_pipe_layout_ci{};
    alloc_pipe_layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    alloc_pipe_layout_ci.setLayoutCount = 1;
    alloc_pipe_layout_ci.pSetLayouts = &alloc_set_layout_;
    alloc_pipe_layout_ci.pushConstantRangeCount = 1;
    alloc_pipe_layout_ci.pPushConstantRanges = &alloc_push;

    if (vkCreatePipelineLayout(vk_device_, &alloc_pipe_layout_ci, nullptr, &alloc_pipeline_layout_) != VK_SUCCESS) {
        return make_error("Failed to create DDGI clipmap alloc pipeline layout");
    }

    {
        auto code = read_spirv(base_path::resolve("shaders/ddgi_clipmap_alloc.comp.spv"));
        VkShaderModule mod = create_shader_module(vk_device_, code);
        VkPipelineShaderStageCreateInfo s{};
        s.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        s.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        s.module = mod;
        s.pName = "main";
        VkComputePipelineCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ci.stage = s;
        ci.layout = alloc_pipeline_layout_;
        VkResult r = vkCreateComputePipelines(vk_device_, resources.pipeline_cache, 1, &ci, nullptr, &clipmap_alloc_pipeline_);
        vkDestroyShaderModule(vk_device_, mod, nullptr);
        if (r != VK_SUCCESS) {
            return make_error("Failed to create DDGI clipmap alloc pipeline");
        }
    }

    // ── Clipmap mark descriptor set layout ──
    // binding 0: DDGIConstants UBO
    // binding 1: depth texture (sampler2D)
    // binding 2: indirection image (storage 3D, read+write)
    std::array<VkDescriptorSetLayoutBinding, 3> mark_bindings{};
    mark_bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    mark_bindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    mark_bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

    // Depth changes with the viewport and the indirection image alternates
    // between the ping-pong halves, so both are rewritten while earlier
    // frames may still reference this set.
    std::array<VkDescriptorBindingFlags, 3> mark_binding_flags{};
    mark_binding_flags[1] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    mark_binding_flags[2] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;

    VkDescriptorSetLayoutBindingFlagsCreateInfo mark_flags_ci{};
    mark_flags_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    mark_flags_ci.bindingCount = static_cast<uint32_t>(mark_binding_flags.size());
    mark_flags_ci.pBindingFlags = mark_binding_flags.data();

    VkDescriptorSetLayoutCreateInfo mark_layout_ci{};
    mark_layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    mark_layout_ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    mark_layout_ci.pNext = &mark_flags_ci;
    mark_layout_ci.bindingCount = static_cast<uint32_t>(mark_bindings.size());
    mark_layout_ci.pBindings = mark_bindings.data();

    if (vkCreateDescriptorSetLayout(vk_device_, &mark_layout_ci, nullptr, &mark_set_layout_) != VK_SUCCESS) {
        return make_error("Failed to create DDGI clipmap mark set layout");
    }

    VkPushConstantRange mark_push{};
    mark_push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    mark_push.offset = 0;
    mark_push.size = sizeof(ClipmapMarkPushConstants);

    VkPipelineLayoutCreateInfo mark_pipe_layout_ci{};
    mark_pipe_layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    mark_pipe_layout_ci.setLayoutCount = 1;
    mark_pipe_layout_ci.pSetLayouts = &mark_set_layout_;
    mark_pipe_layout_ci.pushConstantRangeCount = 1;
    mark_pipe_layout_ci.pPushConstantRanges = &mark_push;

    if (vkCreatePipelineLayout(vk_device_, &mark_pipe_layout_ci, nullptr, &mark_pipeline_layout_) != VK_SUCCESS) {
        return make_error("Failed to create DDGI clipmap mark pipeline layout");
    }

    {
        auto code = read_spirv(base_path::resolve("shaders/ddgi_clipmap_mark.comp.spv"));
        VkShaderModule mod = create_shader_module(vk_device_, code);
        VkPipelineShaderStageCreateInfo s{};
        s.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        s.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        s.module = mod;
        s.pName = "main";
        VkComputePipelineCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ci.stage = s;
        ci.layout = mark_pipeline_layout_;
        VkResult r = vkCreateComputePipelines(vk_device_, resources.pipeline_cache, 1, &ci, nullptr, &clipmap_mark_pipeline_);
        vkDestroyShaderModule(vk_device_, mod, nullptr);
        if (r != VK_SUCCESS) {
            return make_error("Failed to create DDGI clipmap mark pipeline");
        }
    }

    // ── Sampling descriptor set layout ──
    // binding 0: DDGIConstants UBO
    // binding 1: depth texture (sampler2D)
    // binding 2: normal texture (sampler2D)
    // binding 3: irradiance atlas (sampler2D)
    // binding 4: visibility atlas (sampler2D)
    // binding 5: probe offset texture (sampler2D)
    // binding 6: probe status SSBO
    // binding 7: indirect output (storage image)
    std::array<VkDescriptorSetLayoutBinding, 9> sample_bindings{};
    sample_bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    sample_bindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    sample_bindings[2] = {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    sample_bindings[3] = {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    sample_bindings[4] = {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    sample_bindings[5] = {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    sample_bindings[6] = {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    sample_bindings[7] = {7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    sample_bindings[8] = {8, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

    // Enable UPDATE_AFTER_BIND on sampler bindings (1,2 = depth, normal) so they
    // can be rewritten per-frame while the set is referenced by pending command buffers
    std::array<VkDescriptorBindingFlags, 9> sample_binding_flags{};
    sample_binding_flags[1] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; // depth
    sample_binding_flags[2] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; // normal
    // Indirection flips between the two ping-pong halves on every clipmap
    // shift, which can land while an earlier frame's command buffer still
    // references this set.
    sample_binding_flags[8] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;

    VkDescriptorSetLayoutBindingFlagsCreateInfo sample_flags_ci{};
    sample_flags_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    sample_flags_ci.bindingCount = static_cast<uint32_t>(sample_binding_flags.size());
    sample_flags_ci.pBindingFlags = sample_binding_flags.data();

    VkDescriptorSetLayoutCreateInfo sample_layout_ci{};
    sample_layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    sample_layout_ci.pNext = &sample_flags_ci;
    sample_layout_ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    sample_layout_ci.bindingCount = static_cast<uint32_t>(sample_bindings.size());
    sample_layout_ci.pBindings = sample_bindings.data();

    if (vkCreateDescriptorSetLayout(vk_device_, &sample_layout_ci, nullptr, &sample_set_layout_) != VK_SUCCESS) {
        return make_error("Failed to create DDGI sample set layout");
    }

    VkPushConstantRange sample_push{};
    sample_push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    sample_push.offset = 0;
    sample_push.size = sizeof(uint32_t);

    VkPipelineLayoutCreateInfo sample_pipe_layout_ci{};
    sample_pipe_layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    sample_pipe_layout_ci.setLayoutCount = 1;
    sample_pipe_layout_ci.pSetLayouts = &sample_set_layout_;
    sample_pipe_layout_ci.pushConstantRangeCount = 1;
    sample_pipe_layout_ci.pPushConstantRanges = &sample_push;

    if (vkCreatePipelineLayout(vk_device_, &sample_pipe_layout_ci, nullptr, &sample_pipeline_layout_) != VK_SUCCESS) {
        return make_error("Failed to create DDGI sample pipeline layout");
    }

    {
        auto sample_spirv = read_spirv(base_path::resolve("shaders/ddgi_sample_irradiance.comp.spv"));
        VkShaderModule sample_mod = create_shader_module(vk_device_, sample_spirv);
        VkPipelineShaderStageCreateInfo sample_stage{};
        sample_stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        sample_stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        sample_stage.module = sample_mod;
        sample_stage.pName = "main";
        VkComputePipelineCreateInfo sample_ci{};
        sample_ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        sample_ci.stage = sample_stage;
        sample_ci.layout = sample_pipeline_layout_;
        VkResult sample_result = vkCreateComputePipelines(vk_device_, resources.pipeline_cache, 1, &sample_ci, nullptr, &sample_pipeline_);
        vkDestroyShaderModule(vk_device_, sample_mod, nullptr);
        if (sample_result != VK_SUCCESS)
            return make_error("Failed to create DDGI sample irradiance pipeline");
    }

    FJELL_GFX_DEBUG("DDGI pass created (SDF trace + 4 update + sample pipelines)");

    // ── RT trace pipeline (optional) ──
    if (device.ray_tracing_supported()) {
        FJELL_GFX_DEBUG("DDGI: Creating RT trace pipeline...");
        // RT trace set layout:
        // binding 0: DDGIConstants UBO
        // binding 1: TLAS
        // binding 2: radiance output (storage image)
        // binding 3: sky cubemap (samplerCube)
        // binding 4: probe status SSBO
        // binding 5: probe offset texture (sampler2D)
        // binding 6: vertex buffer SSBO (for closest-hit normal computation)
        // binding 7: index buffer SSBO (for closest-hit triangle lookup)
        // binding 8: per-instance data SSBO (material_id, first_index, vertex_offset)
        // binding 9:  irradiance atlas (for multi-bounce GI in closest-hit)
        // binding 10: visibility atlas (for multi-bounce GI in closest-hit)
        // binding 11: scene SDF (raygen burial test)
        std::array<VkDescriptorSetLayoutBinding, 14> rt_bindings{};
        rt_bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                          VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR, nullptr};
        rt_bindings[1] = {1, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1,
                          VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr};
        rt_bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                          VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr};
        rt_bindings[3] = {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                          VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR, nullptr};
        rt_bindings[4] = {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                          VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr};
        rt_bindings[5] = {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                          VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, nullptr};
        rt_bindings[6] = {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                          VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, nullptr};
        rt_bindings[7] = {7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                          VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, nullptr};
        rt_bindings[8] = {8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                          VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, nullptr};
        rt_bindings[9] = {9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                          VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, nullptr};
        rt_bindings[10] = {10, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                           VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, nullptr};
        rt_bindings[11] = {11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr};
        // 12: slot -> grid coord, read by the raygen to place each probe
        rt_bindings[12] = {12, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr};
        // 13: grid coord -> slot, read by the hit shader's cage lookup
        rt_bindings[13] = {13, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                           VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, nullptr};

        // Enable UPDATE_AFTER_BIND on bindings updated every frame
        std::array<VkDescriptorBindingFlags, 14> rt_bind_flags{};
        rt_bind_flags[1] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; // TLAS
        rt_bind_flags[6] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; // vertex buffer
        rt_bind_flags[7] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; // index buffer
        rt_bind_flags[8] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; // per-instance data
        rt_bind_flags[13] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; // ddgi indirection (ping-pong)

        VkDescriptorSetLayoutBindingFlagsCreateInfo rt_flags_ci{};
        rt_flags_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        rt_flags_ci.bindingCount = static_cast<uint32_t>(rt_bind_flags.size());
        rt_flags_ci.pBindingFlags = rt_bind_flags.data();

        VkDescriptorSetLayoutCreateInfo rt_layout_ci{};
        rt_layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        rt_layout_ci.pNext = &rt_flags_ci;
        rt_layout_ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        rt_layout_ci.bindingCount = static_cast<uint32_t>(rt_bindings.size());
        rt_layout_ci.pBindings = rt_bindings.data();

        if (vkCreateDescriptorSetLayout(vk_device_, &rt_layout_ci, nullptr, &rt_trace_set_layout_) != VK_SUCCESS) {
            return make_error("Failed to create DDGI RT trace set layout");
        }

        // RT pipeline layout: set 0 = RT trace set, set 1 = bindless set (for closest-hit material access)
        VkPushConstantRange rt_push{};
        rt_push.stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR;
        rt_push.offset = 0;
        rt_push.size = sizeof(RtTracePushConstants);

        std::array rt_set_layouts = {rt_trace_set_layout_, resources.material_set_layout};

        VkPipelineLayoutCreateInfo rt_pipe_layout_ci{};
        rt_pipe_layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        rt_pipe_layout_ci.setLayoutCount = static_cast<uint32_t>(rt_set_layouts.size());
        rt_pipe_layout_ci.pSetLayouts = rt_set_layouts.data();
        rt_pipe_layout_ci.pushConstantRangeCount = 1;
        rt_pipe_layout_ci.pPushConstantRanges = &rt_push;

        if (vkCreatePipelineLayout(vk_device_, &rt_pipe_layout_ci, nullptr, &rt_trace_pipeline_layout_) != VK_SUCCESS) {
            return make_error("Failed to create DDGI RT trace pipeline layout");
        }

        // Load RT shader modules
        auto rgen_spirv = read_spirv(base_path::resolve("shaders/ddgi_probe_trace_rt.rgen.spv"));
        auto rchit_spirv = read_spirv(base_path::resolve("shaders/ddgi_probe_trace_rt.rchit.spv"));
        auto rmiss_spirv = read_spirv(base_path::resolve("shaders/ddgi_probe_trace_rt.rmiss.spv"));

        VkShaderModule rgen_mod = create_shader_module(vk_device_, rgen_spirv);
        VkShaderModule rchit_mod = create_shader_module(vk_device_, rchit_spirv);
        VkShaderModule rmiss_mod = create_shader_module(vk_device_, rmiss_spirv);

        std::array<VkPipelineShaderStageCreateInfo, 3> rt_stages{};
        rt_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        rt_stages[0].stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
        rt_stages[0].module = rgen_mod;
        rt_stages[0].pName = "main";

        rt_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        rt_stages[1].stage = VK_SHADER_STAGE_MISS_BIT_KHR;
        rt_stages[1].module = rmiss_mod;
        rt_stages[1].pName = "main";

        rt_stages[2].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        rt_stages[2].stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
        rt_stages[2].module = rchit_mod;
        rt_stages[2].pName = "main";

        // Shader groups: raygen, miss, hit
        std::array<VkRayTracingShaderGroupCreateInfoKHR, 3> groups{};
        // Raygen group
        groups[0].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
        groups[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        groups[0].generalShader = 0;
        groups[0].closestHitShader = VK_SHADER_UNUSED_KHR;
        groups[0].anyHitShader = VK_SHADER_UNUSED_KHR;
        groups[0].intersectionShader = VK_SHADER_UNUSED_KHR;

        // Miss group
        groups[1].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
        groups[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        groups[1].generalShader = 1;
        groups[1].closestHitShader = VK_SHADER_UNUSED_KHR;
        groups[1].anyHitShader = VK_SHADER_UNUSED_KHR;
        groups[1].intersectionShader = VK_SHADER_UNUSED_KHR;

        // Closest-hit group
        groups[2].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
        groups[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
        groups[2].generalShader = VK_SHADER_UNUSED_KHR;
        groups[2].closestHitShader = 2;
        groups[2].anyHitShader = VK_SHADER_UNUSED_KHR;
        groups[2].intersectionShader = VK_SHADER_UNUSED_KHR;

        VkRayTracingPipelineCreateInfoKHR rt_ci{};
        rt_ci.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
        rt_ci.stageCount = static_cast<uint32_t>(rt_stages.size());
        rt_ci.pStages = rt_stages.data();
        rt_ci.groupCount = static_cast<uint32_t>(groups.size());
        rt_ci.pGroups = groups.data();
        rt_ci.maxPipelineRayRecursionDepth = 1;
        rt_ci.layout = rt_trace_pipeline_layout_;

        VkResult rt_result = device.create_rt_pipelines_fn()(
            vk_device_, VK_NULL_HANDLE, resources.pipeline_cache, 1, &rt_ci, nullptr, &rt_pipeline_);

        vkDestroyShaderModule(vk_device_, rgen_mod, nullptr);
        vkDestroyShaderModule(vk_device_, rchit_mod, nullptr);
        vkDestroyShaderModule(vk_device_, rmiss_mod, nullptr);

        if (rt_result != VK_SUCCESS) {
            FJELL_GFX_WARN("Failed to create DDGI RT pipeline, falling back to SDF");
            backend_ = DdgiBackend::sdf_fallback;
        } else {
            // Build shader binding table
            VkPhysicalDeviceRayTracingPipelinePropertiesKHR rt_props{};
            rt_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
            VkPhysicalDeviceProperties2 props2{};
            props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            props2.pNext = &rt_props;
            vkGetPhysicalDeviceProperties2(device.physical_device(), &props2);

            uint32_t handle_size = rt_props.shaderGroupHandleSize;
            uint32_t handle_alignment = rt_props.shaderGroupHandleAlignment;
            uint32_t base_alignment = rt_props.shaderGroupBaseAlignment;
            uint32_t handle_size_aligned = (handle_size + handle_alignment - 1) & ~(handle_alignment - 1);

            uint32_t group_count = static_cast<uint32_t>(groups.size());
            std::vector<uint8_t> handles(group_count * handle_size);
            device.get_rt_shader_group_handles_fn()(
                vk_device_, rt_pipeline_, 0, group_count,
                handles.size(), handles.data());

            // SBT layout: [raygen][miss][hit] each base-aligned
            uint32_t raygen_size = (handle_size_aligned + base_alignment - 1) & ~(base_alignment - 1);
            uint32_t miss_size = (handle_size_aligned + base_alignment - 1) & ~(base_alignment - 1);
            uint32_t hit_size = (handle_size_aligned + base_alignment - 1) & ~(base_alignment - 1);
            VkDeviceSize sbt_total = raygen_size + miss_size + hit_size;

            sbt_buffer_ = std::make_unique<Buffer>(
                resources.allocator, sbt_total,
                VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VMA_MEMORY_USAGE_AUTO,
                VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);

            auto* sbt_data = static_cast<uint8_t*>(sbt_buffer_->mapped());
            if (!sbt_data) {
                sbt_buffer_->map();
                sbt_data = static_cast<uint8_t*>(sbt_buffer_->mapped());
            }

            // Write raygen handle
            std::memcpy(sbt_data, handles.data(), handle_size);
            // Write miss handle
            std::memcpy(sbt_data + raygen_size, handles.data() + handle_size, handle_size);
            // Write hit handle
            std::memcpy(sbt_data + raygen_size + miss_size, handles.data() + 2 * handle_size, handle_size);

            VkBufferDeviceAddressInfo addr_info{};
            addr_info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
            addr_info.buffer = sbt_buffer_->handle();
            VkDeviceAddress sbt_addr = device.get_buffer_device_address_fn()(vk_device_, &addr_info);

            sbt_raygen_.deviceAddress = sbt_addr;
            sbt_raygen_.stride = raygen_size;
            sbt_raygen_.size = raygen_size;

            sbt_miss_.deviceAddress = sbt_addr + raygen_size;
            sbt_miss_.stride = miss_size;
            sbt_miss_.size = miss_size;

            sbt_hit_.deviceAddress = sbt_addr + raygen_size + miss_size;
            sbt_hit_.stride = hit_size;
            sbt_hit_.size = hit_size;

            sbt_callable_ = {};

            backend_ = DdgiBackend::ray_traced;
            FJELL_GFX_INFO("DDGI RT pipeline created (raygen + miss + closest-hit)");
        }
    }

    return {};
}

void DdgiPass::destroy() {
    if (vk_device_ == VK_NULL_HANDLE) return;

    auto safe_destroy = [&](auto destroy_fn, auto& handle) {
        if (handle != VK_NULL_HANDLE) { destroy_fn(vk_device_, handle, nullptr); handle = VK_NULL_HANDLE; }
    };

    safe_destroy(vkDestroyPipeline, trace_pipeline_);
    safe_destroy(vkDestroyPipeline, irradiance_update_pipeline_);
    safe_destroy(vkDestroyPipeline, visibility_update_pipeline_);
    safe_destroy(vkDestroyPipeline, probe_offset_pipeline_);
    safe_destroy(vkDestroyPipeline, probe_status_pipeline_);
    safe_destroy(vkDestroyPipeline, sample_pipeline_);
    safe_destroy(vkDestroyPipeline, rt_pipeline_);
    safe_destroy(vkDestroyPipeline, clipmap_alloc_pipeline_);
    safe_destroy(vkDestroyPipeline, clipmap_mark_pipeline_);
    safe_destroy(vkDestroyPipelineLayout, trace_pipeline_layout_);
    safe_destroy(vkDestroyPipelineLayout, update_pipeline_layout_);
    safe_destroy(vkDestroyPipelineLayout, sample_pipeline_layout_);
    safe_destroy(vkDestroyPipelineLayout, rt_trace_pipeline_layout_);
    safe_destroy(vkDestroyPipelineLayout, alloc_pipeline_layout_);
    safe_destroy(vkDestroyPipelineLayout, mark_pipeline_layout_);
    safe_destroy(vkDestroyDescriptorSetLayout, trace_set_layout_);
    safe_destroy(vkDestroyDescriptorSetLayout, update_set_layout_);
    safe_destroy(vkDestroyDescriptorSetLayout, sample_set_layout_);
    safe_destroy(vkDestroyDescriptorSetLayout, alloc_set_layout_);
    safe_destroy(vkDestroyDescriptorSetLayout, mark_set_layout_);
    safe_destroy(vkDestroyDescriptorSetLayout, rt_trace_set_layout_);
    safe_destroy(vkDestroySampler, linear_sampler_);
    sbt_buffer_.reset();

    vk_device_ = VK_NULL_HANDLE;
    device_ = nullptr;
}

void DdgiPass::record(const FrameContext& ctx) {
    if (!config_.enabled) { return; }
    auto* imgs = ctx.pp_resources ? ctx.pp_resources->ddgi() : nullptr;
    if (imgs == nullptr) { return; }
    FJELL_PROFILE_SCOPE_N("record_ddgi");
    FJELL_GPU_ZONE(ctx, "DDGI");
    record_into(ctx, *imgs);
}

/// Write the reallocation set's fixed bindings — the UBO and the three
/// buffers, which point at the same resources for the life of the Images.
/// Called once at init, alongside every other set's static write.
static void write_alloc_static_descriptors(VkDevice device, VkDescriptorSet set,
                                            DdgiPass::Images& imgs) {
    VkDescriptorBufferInfo ubo_info{};
    ubo_info.buffer = imgs.constants_ubo->handle();
    ubo_info.offset = 0;
    ubo_info.range = sizeof(GpuDdgiConstants);

    VkDescriptorBufferInfo slot_grid_info{};
    slot_grid_info.buffer = imgs.slot_grid_buffer->handle();
    slot_grid_info.offset = 0;
    slot_grid_info.range = VK_WHOLE_SIZE;

    VkDescriptorBufferInfo status_info{};
    status_info.buffer = imgs.probe_status_buffer->handle();
    status_info.offset = 0;
    status_info.range = VK_WHOLE_SIZE;

    VkDescriptorBufferInfo free_list_info{};
    free_list_info.buffer = imgs.free_list_buffer->handle();
    free_list_info.offset = 0;
    free_list_info.range = VK_WHOLE_SIZE;

    VkDescriptorBufferInfo last_used_info{};
    last_used_info.buffer = imgs.probe_last_used_buffer->handle();
    last_used_info.offset = 0;
    last_used_info.range = VK_WHOLE_SIZE;

    std::array<VkWriteDescriptorSet, 5> w{};
    for (auto& x : w) { x.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; x.dstSet = set; x.descriptorCount = 1; }
    w[0].dstBinding = 0; w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[0].pBufferInfo = &ubo_info;
    w[1].dstBinding = 3; w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[1].pBufferInfo = &slot_grid_info;
    w[2].dstBinding = 4; w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[2].pBufferInfo = &status_info;
    w[3].dstBinding = 5; w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[3].pBufferInfo = &free_list_info;
    w[4].dstBinding = 6; w[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[4].pBufferInfo = &last_used_info;

    vkUpdateDescriptorSets(device, static_cast<uint32_t>(w.size()), w.data(), 0, nullptr);
}

/// Point the reallocation set's two image bindings at the current ping-pong
/// halves. These are the only bindings that actually change per shift, since
/// which image holds the previous mapping alternates.
static void write_alloc_indirection(VkDevice device, VkDescriptorSet set,
                                     DdgiPass::Images& imgs, int prev_index) {
    VkDescriptorImageInfo prev_info{};
    prev_info.imageView = imgs.indirection_view[prev_index];
    prev_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo next_info{};
    next_info.imageView = imgs.indirection_view[1 - prev_index];
    next_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    std::array<VkWriteDescriptorSet, 2> w{};
    for (auto& x : w) { x.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; x.dstSet = set; x.descriptorCount = 1; }
    w[0].dstBinding = 1; w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[0].pImageInfo = &prev_info;
    w[1].dstBinding = 2; w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[1].pImageInfo = &next_info;

    vkUpdateDescriptorSets(device, static_cast<uint32_t>(w.size()), w.data(), 0, nullptr);
}

/// Rebind the indirection image on every set that reads it. Called after
/// each shift, since the reallocation pass ping-pongs between two images and
/// the one holding the current mapping alternates.
/// The mark set's UBO. Written once at init — the buffer handle is fixed
/// for the life of an Images, so rewriting it per frame would only be a
/// way to touch a set that in-flight command buffers still reference.
static void write_mark_static_descriptors(VkDevice device, DdgiPass::Images& imgs) {
    VkDescriptorBufferInfo ubo_info{};
    ubo_info.buffer = imgs.constants_ubo->handle();
    ubo_info.offset = 0;
    ubo_info.range = sizeof(GpuDdgiConstants);

    VkWriteDescriptorSet w{};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = imgs.mark_set;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w.pBufferInfo = &ubo_info;

    vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
}

/// Point the mark pass at the ping-pong half it should stamp this frame.
/// Unlike the UBO this genuinely alternates, so binding 2 carries
/// UPDATE_AFTER_BIND.
static void write_mark_target(VkDevice device, DdgiPass::Images& imgs, int write_index) {
    VkDescriptorImageInfo target{};
    target.imageView = imgs.indirection_view[write_index];
    target.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet w{};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = imgs.mark_set;
    w.dstBinding = 2;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w.pImageInfo = &target;

    vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
}

static void write_indirection_consumers(VkDevice device, DdgiPass::Images& imgs) {
    VkDescriptorImageInfo current{};
    current.imageView = imgs.indirection_view[imgs.indirection_current];
    current.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    std::array<VkWriteDescriptorSet, 2> w{};
    uint32_t count = 0;

    // Sample set, binding 8 — the screen-space cage lookup.
    if (imgs.sample_set != VK_NULL_HANDLE) {
        w[count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[count].dstSet = imgs.sample_set;
        w[count].dstBinding = 8;
        w[count].descriptorCount = 1;
        w[count].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[count].pImageInfo = &current;
        ++count;
    }

    // RT trace set, binding 13 — the hit shader's cage lookup.
    if (imgs.rt_trace_set != VK_NULL_HANDLE) {
        w[count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[count].dstSet = imgs.rt_trace_set;
        w[count].dstBinding = 13;
        w[count].descriptorCount = 1;
        w[count].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[count].pImageInfo = &current;
        ++count;
    }

    if (count > 0) {
        vkUpdateDescriptorSets(device, count, w.data(), 0, nullptr);
    }
}

static void write_trace_descriptors(VkDevice device, VkDescriptorSet set,
                                     DdgiPass::Images& imgs,
                                     VkImageView sdf_view, VkSampler sdf_sampler,
                                     VkImageView sky_view, VkSampler sky_sampler,
                                     VkSampler linear_sampler) {
    // 0: constants UBO
    VkDescriptorBufferInfo ubo_info{};
    ubo_info.buffer = imgs.constants_ubo->handle();
    ubo_info.offset = 0;
    ubo_info.range = sizeof(GpuDdgiConstants);

    // 1: scene SDF
    VkDescriptorImageInfo sdf_info{};
    sdf_info.sampler = sdf_sampler;
    sdf_info.imageView = sdf_view;
    sdf_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // 2: radiance output (storage)
    VkDescriptorImageInfo radiance_info{};
    radiance_info.imageView = imgs.radiance_view;
    radiance_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    // 3: sky cubemap
    VkDescriptorImageInfo sky_info{};
    sky_info.sampler = sky_sampler;
    sky_info.imageView = sky_view;
    sky_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // 4: probe status SSBO
    VkDescriptorBufferInfo status_info{};
    status_info.buffer = imgs.probe_status_buffer->handle();
    status_info.offset = 0;
    status_info.range = VK_WHOLE_SIZE;

    // 5: probe offset texture
    VkDescriptorImageInfo offset_info{};
    offset_info.sampler = linear_sampler;
    offset_info.imageView = imgs.probe_offset_view;
    offset_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorBufferInfo slot_grid_info{};
    slot_grid_info.buffer = imgs.slot_grid_buffer->handle();
    slot_grid_info.offset = 0;
    slot_grid_info.range = VK_WHOLE_SIZE;

    std::array<VkWriteDescriptorSet, 7> writes{};
    for (auto& w : writes) {
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = set;
        w.descriptorCount = 1;
    }

    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].pBufferInfo = &ubo_info;

    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = &sdf_info;

    writes[2].dstBinding = 2;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[2].pImageInfo = &radiance_info;

    writes[3].dstBinding = 3;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[3].pImageInfo = &sky_info;

    writes[4].dstBinding = 4;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[4].pBufferInfo = &status_info;

    writes[5].dstBinding = 5;
    writes[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[5].pImageInfo = &offset_info;

    writes[6].dstBinding = 6;
    writes[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[6].pBufferInfo = &slot_grid_info;

    vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

bool DdgiPass::recenter_clipmap(Images& imgs, const glm::vec3& camera_position) {
    // Keep the stack's geometry in step with config — level count, spacing
    // and probe counts are all editable in the settings panel, and changing
    // any of them moves every probe.
    imgs.clipmap.configure(config_.clipmap_levels, config_.probe_counts, config_.probe_spacing);
    imgs.clipmap.recenter(camera_position);

    // Which levels have fallen behind their slice of the indirection
    // texture. Levels move independently — a coarse level's cells are large
    // enough that it usually stays put on a frame where level 0 shifts — so
    // this is decided per level rather than for the stack as a whole.
    //
    // Decided here rather than at the dispatch because update_constants()
    // runs first and needs to know: a frame that reallocates anything has to
    // update every probe, not just the round-robin slice.
    imgs.reallocated_this_frame = false;
    for (int i = 0; i < imgs.clipmap.level_count; ++i) {
        bool behind = !imgs.indirection_initialized
                   || imgs.indirection_origin[i] != imgs.clipmap.levels[i].snapped_origin;
        imgs.level_needs_realloc[i] = behind;
        imgs.reallocated_this_frame |= behind;
    }

    // The UBO is rebuilt per viewport right after this, so pushing geometry
    // through config_ is safe even though config_ is shared: each viewport
    // overwrites it with its own clipmap's before update_constants() reads
    // it. grid_position tracks level 0 for the code paths that still want a
    // single representative grid (SDF bounds, debug draw).
    config_.grid_position = imgs.clipmap.levels[0].snapped_origin;

    // Cache the full stack's reach for prepare_accel_structures(), which
    // runs outside command recording and cannot see Images.
    clipmap_bounds_min_ = imgs.clipmap.outer_min();
    clipmap_bounds_max_ = imgs.clipmap.outer_max();

    return imgs.reallocated_this_frame;
}

void DdgiPass::update_constants(Images& imgs, const FrameContext& ctx) {
    GpuDdgiConstants constants{};
    constants.probe_grid_position = config_.grid_position;
    constants.probe_sphere_scale = 0.1f;
    constants.probe_spacing = config_.probe_spacing;
    constants.max_probe_offset = config_.max_probe_offset;
    constants.reciprocal_spacing = 1.0f / config_.probe_spacing;
    constants.self_shadow_bias = config_.self_shadow_bias;
    constants.probe_counts = config_.probe_counts;
    constants.debug_options = static_cast<uint32_t>(config_.debug_mode);

    constants.clipmap_level_count = imgs.clipmap.level_count;
    constants.inv_clipmap_fade_cells = 1.0f;  // one cell of fade, as Lumen uses
    for (int i = 0; i < DDGI_MAX_CLIPMAP_LEVELS; ++i) {
        const auto& lvl = imgs.clipmap.levels[i];
        constants.clipmap_origin_cell_size[i] =
            glm::vec4(lvl.snapped_origin, lvl.cell_size.x);
    }
    constants.irr_tex_w = imgs.irr_width;
    constants.irr_tex_h = imgs.irr_height;
    constants.irr_side = config_.irradiance_side_length;
    constants.probe_rays = config_.probe_rays;
    constants.vis_tex_w = imgs.vis_width;
    constants.vis_tex_h = imgs.vis_height;
    constants.vis_side = config_.visibility_side_length;

    // Round-robin a slice of probes each frame to cut per-frame ray cost.
    // The first frame must seed every probe, and batch == 0 means "all".
    //
    // A frame that reallocates has to cover the probes it reallocated. The
    // batch window is a range of SLOT indices, and the clipmap hands slots
    // out from a free list, so a slot index says nothing about where its
    // probe sits — newly covered probes are scattered across the level's
    // whole block. Leaving them to the round-robin means waiting many
    // frames for the cursor to reach them, and a camera that keeps moving
    // re-marks them faster than that, so freshly covered regions would
    // never get traced and would stay black.
    //
    // Slots are blocked per level, so only the levels that actually moved
    // need widening — which is what makes a stack affordable. Level 0
    // shifts often but is one block; the coarse levels are far larger in
    // world terms and shift rarely.
    int total = imgs.total_probes;
    int per_level = total / std::max(1, imgs.clipmap.level_count);
    int batch = config_.probes_per_frame_batch;

    int realloc_lo = total;
    int realloc_hi = 0;
    if (imgs.reallocated_this_frame) {
        for (int i = 0; i < imgs.clipmap.level_count; ++i) {
            if (!imgs.level_needs_realloc[i]) continue;
            realloc_lo = std::min(realloc_lo, i * per_level);
            realloc_hi = std::max(realloc_hi, (i + 1) * per_level);
        }
    }
    bool has_realloc = realloc_hi > realloc_lo;

    if (is_first_frame_ || batch <= 0 || batch >= total) {
        constants.probe_update_offset = 0;
        constants.probe_update_count = total;
    } else if (has_realloc) {
        constants.probe_update_offset = realloc_lo;
        constants.probe_update_count = realloc_hi - realloc_lo;
    } else {
        if (imgs.probe_update_offset >= total) imgs.probe_update_offset = 0;
        constants.probe_update_offset = imgs.probe_update_offset;
        constants.probe_update_count = std::min(batch, total - imgs.probe_update_offset);
        imgs.probe_update_offset += constants.probe_update_count;
        if (imgs.probe_update_offset >= total) imgs.probe_update_offset = 0;
    }

    constants.hysteresis = config_.hysteresis;
    constants.infinite_bounces_mult = config_.infinite_bounces_mult;
    constants.normal_bias = config_.normal_bias;
    constants.view_bias = config_.view_bias;

    // Random rotation for ray jitter
    static std::mt19937 rng{42};
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);
    float angle = dist(rng) * 2.0f * glm::pi<float>();
    glm::vec3 axis = glm::normalize(glm::vec3(dist(rng) - 0.5f, dist(rng) - 0.5f, dist(rng) - 0.5f));
    constants.random_rotation = glm::rotate(glm::mat4(1.0f), angle, axis);

    constants.camera_position = ctx.camera_position;
    constants.inv_view_proj = ctx.inv_view_proj;
    constants.resolution = glm::vec2(ctx.target ? ctx.target->extent().width : 1920,
                                     ctx.target ? ctx.target->extent().height : 1080);

    std::memcpy(imgs.constants_ubo->mapped(), &constants, sizeof(constants));
}

void DdgiPass::prepare_frame(const FrameContext& ctx, GpuContext& /*gpu*/) {
    if (!ctx.scene) return;

    for (const auto& obj : ctx.scene->objects()) {
        if (const auto* dl = dynamic_cast<const DirectionalLightNode*>(obj.get())) {
            light_direction_ = glm::normalize(-dl->direction);
            light_intensity_ = dl->intensity;
            light_color_ = dl->color;
            break;
        }
    }
}

void DdgiPass::pre_record(const FrameContext& ctx, GpuContext& /*gpu*/) {
    if (!config_.enabled) return;
    // Only build accel structures when DDGI is enabled for this viewport
    if (ctx.pp_resources && ctx.pp_resources->ddgi()) {
        prepare_accel_structures(ctx);
    }
}

AABB DdgiPass::trace_reach_bounds() const {
    // Must span the COARSEST clipmap level, not level 0. This AABB decides
    // which geometry enters the acceleration structure DDGI rays trace
    // against, so sizing it to the fine level leaves the outer levels'
    // probes with nothing to hit — they trace into an empty world and stay
    // dark, which reads as "only the box around the camera is lit".
    glm::vec3 grid_min = clipmap_bounds_min_;
    glm::vec3 grid_max = clipmap_bounds_max_;
    return {grid_min - glm::vec3(MAX_TRACE_DIST), grid_max + glm::vec3(MAX_TRACE_DIST)};
}

void DdgiPass::prepare_accel_structures(const FrameContext& ctx) {
    if (backend_ != DdgiBackend::ray_traced || !accel_manager_ || !ctx.queue || !ctx.mega_buffer)
        return;

    const auto& cmds = ctx.queue->commands();
    AABB bounds = trace_reach_bounds();

    for (const auto& dc : cmds) {
        if (!dc.model) continue;
        if (!dc.world_aabb.intersects(bounds)) continue;
        accel_manager_->ensure_blas(dc.model, *ctx.mega_buffer);
    }

    // Cache particle SSBO for compute dispatch + build particle mesh BLASes
    if (particle_system_) {
        VkBuffer pbuf = particle_system_->particle_buffer_handle();
        if (pbuf != VK_NULL_HANDLE) {
            accel_manager_->set_particle_ssbo(pbuf);
        }

        for (const auto& em : particle_system_->emitters()) {
            if (!em.is_mesh) continue;
            auto shape_key = reinterpret_cast<const void*>(static_cast<uintptr_t>(em.mesh_entry.first_index + 1));
            accel_manager_->ensure_blas_entry(shape_key, em.mesh_entry, *ctx.mega_buffer);
        }
    }
}

void DdgiPass::record_into(const FrameContext& ctx, Images& imgs) {
    if (trace_pipeline_ == VK_NULL_HANDLE) return;
    if (imgs.radiance_image == VK_NULL_HANDLE) return;
    if (ctx.registry == nullptr) return;

    const auto* sdf_res = ctx.registry->find_image("scene_sdf");
    const auto* sky_res = ctx.registry->find_image("sky_cubemap");
    auto sdf_view = sdf_res != nullptr ? sdf_res->view : VK_NULL_HANDLE;
    auto sdf_sampler = sdf_res != nullptr ? sdf_res->sampler : VK_NULL_HANDLE;
    auto sky_cubemap_view = sky_res != nullptr ? sky_res->view : VK_NULL_HANDLE;
    auto sky_cubemap_sampler = sky_res != nullptr ? sky_res->sampler : VK_NULL_HANDLE;

    // Sky cubemap identity changed (HDRI loaded or cleared) — force a
    // descriptor rewrite on trace_set / rt_trace_set. The old handles may
    // already be destroyed, so this must happen before we use them.
    if (sky_cubemap_view != imgs.last_sky_view ||
        sky_cubemap_sampler != imgs.last_sky_sampler) {
        imgs.images_need_init = true;
        imgs.last_sky_view = sky_cubemap_view;
        imgs.last_sky_sampler = sky_cubemap_sampler;
    }

    // Allocate descriptor pool + sets for this Images instance if needed
    if (imgs.desc_pool == VK_NULL_HANDLE) {
        bool has_rt = rt_trace_set_layout_ != VK_NULL_HANDLE;
        // +2 sets beyond the original three: the clipmap reallocation pass
        // (1 UBO, 2 storage images, 4 storage buffers) and the mark pass
        // (1 UBO, 1 sampler, 1 storage image).
        uint32_t max_sets = has_rt ? 6u : 5u;

        std::array<VkDescriptorPoolSize, 5> pool_sizes{};
        pool_sizes[0] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, has_rt ? 6u : 5u};
        pool_sizes[1] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, has_rt ? 15u : 10u};
        pool_sizes[2] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, has_rt ? 11u : 9u};
        pool_sizes[3] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, has_rt ? 13u : 8u};
        pool_sizes[4] = {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, has_rt ? 1u : 0u};
        uint32_t pool_size_count = has_rt ? 5u : 4u;

        VkDescriptorPoolCreateInfo pool_ci{};
        pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_ci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        pool_ci.maxSets = max_sets;
        pool_ci.poolSizeCount = pool_size_count;
        pool_ci.pPoolSizes = pool_sizes.data();

        vkCreateDescriptorPool(vk_device_, &pool_ci, nullptr, &imgs.desc_pool);

        if (has_rt) {
            std::array layouts = {trace_set_layout_, rt_trace_set_layout_, update_set_layout_,
                                  sample_set_layout_, alloc_set_layout_, mark_set_layout_};
            std::array<VkDescriptorSet, 6> sets{};

            VkDescriptorSetAllocateInfo alloc_info{};
            alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            alloc_info.descriptorPool = imgs.desc_pool;
            alloc_info.descriptorSetCount = 6;
            alloc_info.pSetLayouts = layouts.data();

            vkAllocateDescriptorSets(vk_device_, &alloc_info, sets.data());
            imgs.trace_set = sets[0];
            imgs.rt_trace_set = sets[1];
            imgs.update_set = sets[2];
            imgs.sample_set = sets[3];
            imgs.alloc_set = sets[4];
            imgs.mark_set = sets[5];
        } else {
            std::array layouts = {trace_set_layout_, update_set_layout_, sample_set_layout_,
                                  alloc_set_layout_, mark_set_layout_};
            std::array<VkDescriptorSet, 5> sets{};

            VkDescriptorSetAllocateInfo alloc_info{};
            alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            alloc_info.descriptorPool = imgs.desc_pool;
            alloc_info.descriptorSetCount = 5;
            alloc_info.pSetLayouts = layouts.data();

            vkAllocateDescriptorSets(vk_device_, &alloc_info, sets.data());
            imgs.trace_set = sets[0];
            imgs.update_set = sets[1];
            imgs.sample_set = sets[2];
            imgs.alloc_set = sets[3];
            imgs.mark_set = sets[4];
        }
        imgs.images_need_init = true;
    }

    // Follow the camera before the UBO is written, so grid_position in the
    // constants reflects the level's position for the frame that is about
    // to be traced rather than the previous one.
    recenter_clipmap(imgs, ctx.camera_position);

    // Update constants UBO
    update_constants(imgs, ctx);

    // Write descriptors on first use or when views become available
    if (imgs.images_need_init) {
        if (sdf_view == VK_NULL_HANDLE) {
            FJELL_GFX_WARN("DDGI: SDF view not available, skipping");
            frame_counter_++;
            return;
        }
        if (sky_cubemap_view == VK_NULL_HANDLE) {
            FJELL_GFX_WARN("DDGI: Sky cubemap view not available, skipping");
            frame_counter_++;
            return;
        }
        FJELL_GFX_DEBUG("DDGI: Writing descriptors (first init)");
    }
    if (imgs.images_need_init && sdf_view != VK_NULL_HANDLE && sky_cubemap_view != VK_NULL_HANDLE) {
        write_trace_descriptors(vk_device_, imgs.trace_set, imgs,
                                sdf_view, sdf_sampler,
                                sky_cubemap_view, sky_cubemap_sampler,
                                linear_sampler_);

        // Write update set descriptors
        VkDescriptorBufferInfo ubo_info{};
        ubo_info.buffer = imgs.constants_ubo->handle();
        ubo_info.range = sizeof(GpuDdgiConstants);

        VkDescriptorImageInfo radiance_info{};
        radiance_info.sampler = linear_sampler_;
        radiance_info.imageView = imgs.radiance_view;
        radiance_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo irr_info{};
        irr_info.imageView = imgs.irradiance_view;
        irr_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo vis_info{};
        vis_info.imageView = imgs.visibility_view;
        vis_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo offset_info{};
        offset_info.imageView = imgs.probe_offset_view;
        offset_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorBufferInfo status_info{};
        status_info.buffer = imgs.probe_status_buffer->handle();
        status_info.range = VK_WHOLE_SIZE;

        std::array<VkWriteDescriptorSet, 6> writes{};
        for (auto& w : writes) {
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = imgs.update_set;
            w.descriptorCount = 1;
        }
        writes[0].dstBinding = 0; writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; writes[0].pBufferInfo = &ubo_info;
        writes[1].dstBinding = 1; writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[1].pImageInfo = &radiance_info;
        writes[2].dstBinding = 2; writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; writes[2].pImageInfo = &irr_info;
        writes[3].dstBinding = 3; writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; writes[3].pImageInfo = &vis_info;
        writes[4].dstBinding = 4; writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; writes[4].pImageInfo = &offset_info;
        writes[5].dstBinding = 5; writes[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[5].pBufferInfo = &status_info;

        vkUpdateDescriptorSets(vk_device_, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

        // Write sample set static descriptors (irradiance, visibility, offset, status, indirect)
        // Depth + normal are written every frame since they change with viewport resize
        {
            VkDescriptorBufferInfo s_ubo{};
            s_ubo.buffer = imgs.constants_ubo->handle();
            s_ubo.range = sizeof(GpuDdgiConstants);

            VkDescriptorImageInfo s_irr{};
            s_irr.sampler = linear_sampler_;
            s_irr.imageView = imgs.irradiance_view;
            s_irr.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            VkDescriptorImageInfo s_vis{};
            s_vis.sampler = linear_sampler_;
            s_vis.imageView = imgs.visibility_view;
            s_vis.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            VkDescriptorImageInfo s_offset{};
            s_offset.sampler = linear_sampler_;
            s_offset.imageView = imgs.probe_offset_view;
            s_offset.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            VkDescriptorBufferInfo s_status{};
            s_status.buffer = imgs.probe_status_buffer->handle();
            s_status.range = VK_WHOLE_SIZE;

            VkDescriptorImageInfo s_indirect{};
            s_indirect.imageView = imgs.indirect_view;
            s_indirect.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            // Seed binding 8 with whichever half is current. It is rewritten
            // on every shift by write_indirection_consumers(), but the set
            // must be complete before the first dispatch either way.
            VkDescriptorImageInfo s_indirection{};
            s_indirection.imageView = imgs.indirection_view[imgs.indirection_current];
            s_indirection.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            std::array<VkWriteDescriptorSet, 7> sw{};
            for (auto& w : sw) { w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = imgs.sample_set; w.descriptorCount = 1; }
            sw[0].dstBinding = 0; sw[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; sw[0].pBufferInfo = &s_ubo;
            sw[1].dstBinding = 3; sw[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sw[1].pImageInfo = &s_irr;
            sw[2].dstBinding = 4; sw[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sw[2].pImageInfo = &s_vis;
            sw[3].dstBinding = 5; sw[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sw[3].pImageInfo = &s_offset;
            sw[4].dstBinding = 6; sw[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; sw[4].pBufferInfo = &s_status;
            sw[5].dstBinding = 7; sw[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; sw[5].pImageInfo = &s_indirect;
            sw[6].dstBinding = 8; sw[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; sw[6].pImageInfo = &s_indirection;

            vkUpdateDescriptorSets(vk_device_, static_cast<uint32_t>(sw.size()), sw.data(), 0, nullptr);
        }

        // Write RT trace descriptors (static bindings only — TLAS updated per-frame)
        if (imgs.rt_trace_set != VK_NULL_HANDLE) {
            VkDescriptorBufferInfo rt_ubo{};
            rt_ubo.buffer = imgs.constants_ubo->handle();
            rt_ubo.range = sizeof(GpuDdgiConstants);

            VkDescriptorImageInfo rt_radiance{};
            rt_radiance.imageView = imgs.radiance_view;
            rt_radiance.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            VkDescriptorImageInfo rt_sky{};
            rt_sky.sampler = sky_cubemap_sampler;
            rt_sky.imageView = sky_cubemap_view;
            rt_sky.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkDescriptorBufferInfo rt_status{};
            rt_status.buffer = imgs.probe_status_buffer->handle();
            rt_status.range = VK_WHOLE_SIZE;

            VkDescriptorImageInfo rt_offset{};
            rt_offset.sampler = linear_sampler_;
            rt_offset.imageView = imgs.probe_offset_view;
            rt_offset.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            // Irradiance + visibility atlases for multi-bounce GI in closest-hit
            VkDescriptorImageInfo rt_irradiance{};
            rt_irradiance.sampler = linear_sampler_;
            rt_irradiance.imageView = imgs.irradiance_view;
            rt_irradiance.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            VkDescriptorImageInfo rt_visibility{};
            rt_visibility.sampler = linear_sampler_;
            rt_visibility.imageView = imgs.visibility_view;
            rt_visibility.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            // Scene SDF — raygen burial test
            VkDescriptorImageInfo rt_sdf{};
            rt_sdf.sampler = sdf_sampler;
            rt_sdf.imageView = sdf_view;
            rt_sdf.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkDescriptorBufferInfo rt_slot_grid{};
            rt_slot_grid.buffer = imgs.slot_grid_buffer->handle();
            rt_slot_grid.range = VK_WHOLE_SIZE;

            VkDescriptorImageInfo rt_indirection{};
            rt_indirection.imageView = imgs.indirection_view[imgs.indirection_current];
            rt_indirection.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            // Write bindings 0, 2-5, 9-13 (skip 1=TLAS, 6-8=buffers, updated per-frame)
            std::array<VkWriteDescriptorSet, 10> rtw{};
            for (auto& w : rtw) { w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = imgs.rt_trace_set; w.descriptorCount = 1; }
            rtw[0].dstBinding = 0; rtw[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; rtw[0].pBufferInfo = &rt_ubo;
            rtw[1].dstBinding = 2; rtw[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; rtw[1].pImageInfo = &rt_radiance;
            rtw[2].dstBinding = 3; rtw[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; rtw[2].pImageInfo = &rt_sky;
            rtw[3].dstBinding = 4; rtw[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; rtw[3].pBufferInfo = &rt_status;
            rtw[4].dstBinding = 5; rtw[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; rtw[4].pImageInfo = &rt_offset;
            rtw[5].dstBinding = 9; rtw[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; rtw[5].pImageInfo = &rt_irradiance;
            rtw[6].dstBinding = 10; rtw[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; rtw[6].pImageInfo = &rt_visibility;
            rtw[7].dstBinding = 11; rtw[7].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; rtw[7].pImageInfo = &rt_sdf;
            rtw[8].dstBinding = 12; rtw[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; rtw[8].pBufferInfo = &rt_slot_grid;
            rtw[9].dstBinding = 13; rtw[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; rtw[9].pImageInfo = &rt_indirection;

            vkUpdateDescriptorSets(vk_device_, static_cast<uint32_t>(rtw.size()), rtw.data(), 0, nullptr);
        }

        write_alloc_static_descriptors(vk_device_, imgs.alloc_set, imgs);
        write_mark_static_descriptors(vk_device_, imgs);

        is_first_frame_ = true;
        imgs.images_need_init = false;
    }

    // SDF backend requires the SDF view
    if (backend_ == DdgiBackend::sdf_fallback && sdf_view == VK_NULL_HANDLE) {
        frame_counter_++;
        return;
    }

    auto barrier_image = [&](VkImage image, VkImageLayout old_layout, VkImageLayout new_layout,
                             VkAccessFlags2 src_access, VkAccessFlags2 dst_access) {
        VkImageMemoryBarrier2 b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b.srcAccessMask = src_access;
        b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b.dstAccessMask = dst_access;
        b.oldLayout = old_layout;
        b.newLayout = new_layout;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo d{};
        d.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        d.imageMemoryBarrierCount = 1;
        d.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(ctx.cmd, &d);
    };

    auto barrier_memory = [&]() {
        VkMemoryBarrier2 m{};
        m.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        m.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        m.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        m.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
        VkDependencyInfo d{};
        d.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        d.memoryBarrierCount = 1;
        d.pMemoryBarriers = &m;
        vkCmdPipelineBarrier2(ctx.cmd, &d);
    };

    GpuDdgiConstants* constants = static_cast<GpuDdgiConstants*>(imgs.constants_ubo->mapped());

    // ── 0. Initialize probe status buffer on first use ──
    // Must stay ahead of the reallocation pass below: that pass marks
    // freshly-allocated slots UNINITIALIZED, and this fill would erase
    // those marks if it ran after.
    if (is_first_frame_) {
        constexpr uint32_t PROBE_STATUS_ACTIVE = 4;
        VkDeviceSize status_size = static_cast<VkDeviceSize>(imgs.total_probes) * sizeof(uint32_t);
        vkCmdFillBuffer(ctx.cmd, imgs.probe_status_buffer->handle(), 0, status_size, PROBE_STATUS_ACTIVE);
        // Zero rather than the current frame number, so nothing is treated
        // as recently-used before anything has actually marked it.
        vkCmdFillBuffer(ctx.cmd, imgs.probe_last_used_buffer->handle(), 0, status_size, 0);
        // free_count and high_water both start empty: no slot has been
        // freed and none has been handed out.
        vkCmdFillBuffer(ctx.cmd, imgs.free_list_buffer->handle(), 0, 2 * sizeof(uint32_t), 0);

        VkMemoryBarrier2 fill_barrier{};
        fill_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        fill_barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        fill_barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        fill_barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        fill_barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
        VkDependencyInfo fill_dep{};
        fill_dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        fill_dep.memoryBarrierCount = 1;
        fill_dep.pMemoryBarriers = &fill_barrier;
        vkCmdPipelineBarrier2(ctx.cmd, &fill_dep);
    }

    // ── 0.5 Clipmap mark + reallocation ──
    // Runs every frame, not only when a level crosses a cell boundary. With
    // demand-driven allocation the set of wanted probes changes whenever the
    // view changes — turning on the spot reveals cells nothing had marked —
    // so the mapping has to be rebuilt continuously. What makes that cheap
    // is that REUSE carries converged probes straight across: a frame where
    // nothing moved re-derives the same mapping and retraces nothing.
    {
        FJELL_PROFILE_SCOPE_N("ddgi_clipmap_alloc");

        // Nothing to carry over the first time this viewport builds its
        // mapping. Beyond that the REUSE phase decides per probe whether it
        // is still covered, so a large jump needs no special case — every
        // probe simply fails the coverage test and lands on the free list.
        bool full_reset = !imgs.indirection_initialized;

        // Both indirection images need to be readable/writable. On the first
        // frame neither has ever been written, so their contents are
        // undefined — which is fine, because a full reset ignores the
        // previous mapping entirely.
        for (int i = 0; i < 2; ++i) {
            barrier_image(imgs.indirection_image[i],
                          imgs.indirection_initialized ? VK_IMAGE_LAYOUT_GENERAL
                                                       : VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_GENERAL,
                          VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
                          VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
        }
        imgs.indirection_initialized = true;

        // The image currently bound for sampling holds the mapping this
        // rebuild starts from, so it is what the pass reads; the new mapping
        // goes into the other one, which then becomes current. Reading from
        // `1 - current` instead would pull the mapping from two rebuilds ago
        // and write over the one still in use.
        int prev_index = imgs.indirection_current;
        write_alloc_indirection(vk_device_, imgs.alloc_set, imgs, prev_index);

        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, clipmap_alloc_pipeline_);
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, alloc_pipeline_layout_,
                                0, 1, &imgs.alloc_set, 0, nullptr);

        glm::ivec3 counts = config_.probe_counts;
        uint32_t gx = static_cast<uint32_t>((counts.x + 3) / 4);
        uint32_t gy = static_cast<uint32_t>((counts.y + 3) / 4);
        uint32_t gz = static_cast<uint32_t>((counts.z + 3) / 4);

        auto dispatch_phase = [&](int level, uint32_t phase) {
            ClipmapAllocPushConstants pc{};
            pc.prev_origin = imgs.indirection_origin[level];
            pc.level = level;
            pc.phase = phase;
            // Only the very first build has no previous mapping to read.
            // After that every level always has one, whether or not it moved.
            pc.full_reset = full_reset ? 1u : 0u;
            pc.frame_number = frame_counter_;
            pc.frames_to_keep =
                static_cast<uint32_t>(std::max(0, config_.frames_to_keep_unused_probes));
            vkCmdPushConstants(ctx.cmd, alloc_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                               0, sizeof(pc), &pc);
            vkCmdDispatch(ctx.cmd, gx, gy, gz);
            barrier_memory();
        };

        // CLEAR every level first. The mark pass that follows stamps cells
        // across all levels in one go, so it cannot run until the whole
        // destination image is INVALID — clearing a level afterwards would
        // wipe marks already placed in it.
        for (int level = 0; level < imgs.clipmap.level_count; ++level) {
            dispatch_phase(level, ALLOC_PHASE_CLEAR);
        }

        // MARK: stamp USED onto the cells this frame will actually read.
        {
            FJELL_PROFILE_SCOPE_N("ddgi_clipmap_mark");
            write_mark_target(vk_device_, imgs, 1 - prev_index);

            VkImageView depth_view = ctx.target ? ctx.target->depth_view() : VK_NULL_HANDLE;
            if (depth_view != VK_NULL_HANDLE) {
                VkDescriptorImageInfo d{};
                d.sampler = linear_sampler_;
                d.imageView = depth_view;
                d.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
                VkWriteDescriptorSet w{};
                w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w.dstSet = imgs.mark_set;
                w.dstBinding = 1;
                w.descriptorCount = 1;
                w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w.pImageInfo = &d;
                vkUpdateDescriptorSets(vk_device_, 1, &w, 0, nullptr);

                vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, clipmap_mark_pipeline_);
                vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                        mark_pipeline_layout_, 0, 1, &imgs.mark_set, 0, nullptr);

                ClipmapMarkPushConstants mpc{};
                mpc.output_resolution_half = config_.half_res_output ? 1u : 0u;
                vkCmdPushConstants(ctx.cmd, mark_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                                   0, sizeof(mpc), &mpc);
                vkCmdDispatch(ctx.cmd,
                              (imgs.indirect_extent.width + 7) / 8,
                              (imgs.indirect_extent.height + 7) / 8, 1);
                barrier_memory();

                vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, clipmap_alloc_pipeline_);
                vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                        alloc_pipeline_layout_, 0, 1, &imgs.alloc_set, 0, nullptr);
            }
        }

        // REUSE then ALLOC, per level. The free list is per level: a level
        // must only reclaim slots it freed itself, or one level would hand
        // out another's slots and the two pools would interleave.
        for (int level = 0; level < imgs.clipmap.level_count; ++level) {
            // Only free_count (offset 0). high_water sits at offset 4 and
            // must survive: it records how much of the pool has ever been
            // handed out, so zeroing it would re-issue slots already in use.
            vkCmdFillBuffer(ctx.cmd, imgs.free_list_buffer->handle(), 0, sizeof(uint32_t), 0);
            barrier_memory();

            dispatch_phase(level, ALLOC_PHASE_REUSE);
            dispatch_phase(level, ALLOC_PHASE_ALLOC);

            imgs.indirection_origin[level] = imgs.clipmap.levels[level].snapped_origin;
        }

        imgs.indirection_current = 1 - prev_index;

        // Point every consumer of the indirection texture at the half that
        // was just written. These sets are otherwise only written at init,
        // but which image is current alternates with each shift.
        write_indirection_consumers(vk_device_, imgs);
    }

    // ── 1. Pre-trace barriers ──
    barrier_image(imgs.radiance_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                  VK_ACCESS_2_SHADER_READ_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
    barrier_image(imgs.probe_offset_image,
                  is_first_frame_ ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_2_SHADER_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT);

    // Irradiance + visibility must be in GENERAL before the trace reads them (multi-bounce)
    if (is_first_frame_) {
        barrier_image(imgs.irradiance_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                      0, VK_ACCESS_2_SHADER_READ_BIT);
        barrier_image(imgs.visibility_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                      0, VK_ACCESS_2_SHADER_READ_BIT);
    }

    // ── 2. Build TLAS + dispatch trace (RT or SDF) ──
    // Build TLAS if RT backend is active (BLAS building done in prepare_accel_structures)
    if (backend_ == DdgiBackend::ray_traced && accel_manager_ && ctx.queue && ctx.mega_buffer) {
        FJELL_PROFILE_SCOPE_N("ddgi_tlas_build");
        FJELL_GPU_ZONE(ctx, "DDGI TLAS Build");
        const auto& cmds = ctx.queue->commands();
        if (!cmds.empty()) {
            // Only include objects that rays can reach from the probe grid
            AABB bounds = trace_reach_bounds();

            std::vector<GpuDrawData> tlas_draws;
            std::vector<const Model*> tlas_models;
            tlas_draws.reserve(cmds.size());
            tlas_models.reserve(cmds.size());
            for (const auto& dc : cmds) {
                if (!dc.model) continue;
                if (!dc.world_aabb.intersects(bounds)) continue;
                GpuDrawData data{};
                data.model = dc.push.model;
                data.material_id = dc.push.material_id;
                tlas_draws.push_back(data);
                tlas_models.push_back(dc.model);
            }
            // Collect mesh particle emitters for per-particle TLAS instances
            std::vector<AccelStructureManager::ParticleEmitterInfo> particle_infos;
            if (particle_system_) {
                for (const auto& em : particle_system_->emitters()) {
                    if (!em.is_mesh) continue;
                    auto shape_key = reinterpret_cast<const void*>(static_cast<uintptr_t>(em.mesh_entry.first_index + 1));
                    const auto* blas = accel_manager_->find_blas(shape_key);
                    if (!blas) continue;

                    AccelStructureManager::ParticleEmitterInfo info{};
                    info.base_offset = em.base_offset;
                    info.max_particles = em.max_particles;
                    info.material_id = em.mesh_material_id;
                    info.avg_particle_size = 1.0f;
                    info.blas = blas;
                    particle_infos.push_back(info);
                }
            }

            if (!tlas_draws.empty() || !particle_infos.empty()) {
                accel_manager_->build_tlas(ctx.cmd, ctx.frame_index, tlas_draws.data(),
                                           static_cast<uint32_t>(tlas_draws.size()),
                                           tlas_models, particle_infos);
            }
        }
    }

    // Keep vertex/index buffer descriptors current (MegaBuffer may reallocate)
    if (imgs.rt_trace_set != VK_NULL_HANDLE && ctx.mega_buffer) {
        VkDescriptorBufferInfo verts_info{};
        verts_info.buffer = ctx.mega_buffer->vertex_buffer_handle();
        verts_info.range = VK_WHOLE_SIZE;

        VkDescriptorBufferInfo indices_info{};
        indices_info.buffer = ctx.mega_buffer->index_buffer_handle();
        indices_info.range = VK_WHOLE_SIZE;

        VkWriteDescriptorSet verts_write{};
        verts_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        verts_write.dstSet = imgs.rt_trace_set;
        verts_write.dstBinding = 6;
        verts_write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        verts_write.descriptorCount = 1;
        verts_write.pBufferInfo = &verts_info;

        VkWriteDescriptorSet indices_write{};
        indices_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        indices_write.dstSet = imgs.rt_trace_set;
        indices_write.dstBinding = 7;
        indices_write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        indices_write.descriptorCount = 1;
        indices_write.pBufferInfo = &indices_info;

        std::array buf_writes = {verts_write, indices_write};
        vkUpdateDescriptorSets(vk_device_, static_cast<uint32_t>(buf_writes.size()), buf_writes.data(), 0, nullptr);
    }

    bool use_rt = backend_ == DdgiBackend::ray_traced
                  && rt_pipeline_ != VK_NULL_HANDLE
                  && accel_manager_ && accel_manager_->tlas_valid(ctx.frame_index)
                  && imgs.rt_trace_set != VK_NULL_HANDLE;

    if (use_rt) {
        FJELL_PROFILE_SCOPE_N("ddgi_trace_rt");
        FJELL_GPU_ZONE(ctx, "DDGI Trace RT");
        // Update TLAS descriptor (changes every frame)
        VkWriteDescriptorSetAccelerationStructureKHR tlas_write_as{};
        tlas_write_as.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
        tlas_write_as.accelerationStructureCount = 1;
        VkAccelerationStructureKHR tlas = accel_manager_->tlas(ctx.frame_index);
        tlas_write_as.pAccelerationStructures = &tlas;

        VkWriteDescriptorSet tlas_write{};
        tlas_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        tlas_write.dstSet = imgs.rt_trace_set;
        tlas_write.dstBinding = 1;
        tlas_write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        tlas_write.descriptorCount = 1;
        tlas_write.pNext = &tlas_write_as;

        // Per-instance data (material + geometry offsets)
        VkDescriptorBufferInfo inst_data_info{};
        inst_data_info.buffer = accel_manager_->instance_data_buffer(ctx.frame_index);
        inst_data_info.range = VK_WHOLE_SIZE;

        VkWriteDescriptorSet inst_write{};
        inst_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        inst_write.dstSet = imgs.rt_trace_set;
        inst_write.dstBinding = 8;
        inst_write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        inst_write.descriptorCount = 1;
        inst_write.pBufferInfo = &inst_data_info;

        std::array writes = {tlas_write, inst_write};
        vkUpdateDescriptorSets(vk_device_, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

        // Bind RT pipeline and descriptors
        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rt_pipeline_);
        std::array rt_sets = {imgs.rt_trace_set, ctx.bindless_set};
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                                rt_trace_pipeline_layout_, 0,
                                static_cast<uint32_t>(rt_sets.size()), rt_sets.data(),
                                0, nullptr);

        RtTracePushConstants rt_push{};
        rt_push.light_direction = light_direction_;
        rt_push.light_intensity = light_intensity_;
        rt_push.light_color = light_color_;
        rt_push.max_trace_dist = MAX_TRACE_DIST;
        rt_push.emissive_gi_boost = config_.emissive_gi_boost;
        rt_push.sdf_volume_min = sdf_volume_min_;
        rt_push.sdf_volume_max = sdf_volume_max_;

        vkCmdPushConstants(ctx.cmd, rt_trace_pipeline_layout_,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR,
                           0, sizeof(rt_push), &rt_push);

        uint32_t width = static_cast<uint32_t>(config_.probe_rays);
        uint32_t height = static_cast<uint32_t>(constants->probe_update_count);
        device_->cmd_trace_rays_fn()(ctx.cmd,
                                     &sbt_raygen_, &sbt_miss_, &sbt_hit_, &sbt_callable_,
                                     width, height, 1);
    } else if (backend_ == DdgiBackend::sdf_fallback) {
        FJELL_PROFILE_SCOPE_N("ddgi_trace_sdf");
        FJELL_GPU_ZONE(ctx, "DDGI Trace SDF");
        // SDF fallback trace (only when explicitly configured for SDF)
        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, trace_pipeline_);
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                trace_pipeline_layout_, 0, 1, &imgs.trace_set, 0, nullptr);

        TracePushConstants push{};
        push.light_direction = light_direction_;
        push.light_intensity = light_intensity_;
        push.light_color = light_color_;
        push.sdf_resolution = static_cast<float>(SceneSdfPass::DEFAULT_RESOLUTION);
        // Whole stack, not level 0 — probes on the coarse levels sit well
        // outside the fine level's box and need the same volume.
        push.sdf_volume_min = imgs.clipmap.outer_min();
        push.sdf_volume_max = imgs.clipmap.outer_max();

        vkCmdPushConstants(ctx.cmd, trace_pipeline_layout_,
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);

        uint32_t groups_x = (static_cast<uint32_t>(config_.probe_rays) + 31) / 32;
        uint32_t groups_y = static_cast<uint32_t>(constants->probe_update_count);
        vkCmdDispatch(ctx.cmd, groups_x, groups_y, 1);
    } else {
        // RT backend but TLAS not ready — skip trace this frame, keep previous radiance
        frame_counter_++;
        return;
    }

    // ── 3. Barrier: radiance write → read ──
    if (use_rt) {
        // RT writes from ray tracing stage, reads in compute stage
        VkImageMemoryBarrier2 b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.srcStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
        b.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.image = imgs.radiance_image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo d{};
        d.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        d.imageMemoryBarrierCount = 1;
        d.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(ctx.cmd, &d);
    } else {
        barrier_image(imgs.radiance_image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_ACCESS_2_SHADER_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    }

    // ── 4. Transition irradiance/visibility to GENERAL for update ──
    barrier_image(imgs.irradiance_image,
                  is_first_frame_ ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                  VK_IMAGE_LAYOUT_GENERAL,
                  VK_ACCESS_2_SHADER_READ_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
    barrier_image(imgs.visibility_image,
                  is_first_frame_ ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                  VK_IMAGE_LAYOUT_GENERAL,
                  VK_ACCESS_2_SHADER_READ_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);

    struct UpdatePush {
        uint32_t first_frame;
        float max_trace_dist;
    } update_push{is_first_frame_ ? 1U : 0U, MAX_TRACE_DIST};

    // ── 5. Dispatch irradiance update ──
    {
        FJELL_PROFILE_SCOPE_N("ddgi_update_irradiance");
        FJELL_GPU_ZONE(ctx, "DDGI Update Irradiance");
        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, irradiance_update_pipeline_);
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                update_pipeline_layout_, 0, 1, &imgs.update_set, 0, nullptr);
        vkCmdPushConstants(ctx.cmd, update_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(update_push), &update_push);
        vkCmdDispatch(ctx.cmd,
                      (static_cast<uint32_t>(imgs.irr_width) + 7) / 8,
                      (static_cast<uint32_t>(imgs.irr_height) + 7) / 8, 1);
    }

    // ── 6. Dispatch visibility update ──
    {
        FJELL_PROFILE_SCOPE_N("ddgi_update_visibility");
        FJELL_GPU_ZONE(ctx, "DDGI Update Visibility");
        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, visibility_update_pipeline_);
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                update_pipeline_layout_, 0, 1, &imgs.update_set, 0, nullptr);
        vkCmdPushConstants(ctx.cmd, update_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(update_push), &update_push);
        vkCmdDispatch(ctx.cmd,
                      (static_cast<uint32_t>(imgs.vis_width) + 7) / 8,
                      (static_cast<uint32_t>(imgs.vis_height) + 7) / 8, 1);
    }

    // ── 7. Barrier for probe offset/status computation ──
    barrier_memory();
    barrier_image(imgs.probe_offset_image,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                  VK_ACCESS_2_SHADER_READ_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);

    // ── 8. Dispatch probe offsets ──
    uint32_t total = static_cast<uint32_t>(imgs.total_probes);
    {
        FJELL_PROFILE_SCOPE_N("ddgi_probe_offsets");
        FJELL_GPU_ZONE(ctx, "DDGI Probe Offsets");
        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, probe_offset_pipeline_);
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                update_pipeline_layout_, 0, 1, &imgs.update_set, 0, nullptr);
        vkCmdPushConstants(ctx.cmd, update_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(update_push), &update_push);
        vkCmdDispatch(ctx.cmd, (total + 31) / 32, 1, 1);
    }

    // ── 9. Dispatch probe status ──
    {
        FJELL_PROFILE_SCOPE_N("ddgi_probe_status");
        FJELL_GPU_ZONE(ctx, "DDGI Probe Status");
        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, probe_status_pipeline_);
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                update_pipeline_layout_, 0, 1, &imgs.update_set, 0, nullptr);
        vkCmdPushConstants(ctx.cmd, update_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(update_push), &update_push);
        vkCmdDispatch(ctx.cmd, (total + 31) / 32, 1, 1);
    }

    // ── 10. Transition indirect image to GENERAL for sample output ──
    barrier_image(imgs.indirect_image,
                  is_first_frame_ ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_IMAGE_LAYOUT_GENERAL,
                  VK_ACCESS_2_SHADER_READ_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);

    // ── 11. Dispatch fullscreen sampling (skip first frame — depth buffer not yet rendered) ──
    if (!is_first_frame_) {
        FJELL_PROFILE_SCOPE_N("ddgi_sample");
        FJELL_GPU_ZONE(ctx, "DDGI Sample");
        // Rewrite the depth/normal sample descriptors only when the views
        // change (on viewport resize) — they're stable frame to frame, so the
        // per-frame UPDATE_AFTER_BIND write is wasted work otherwise. Mirrors
        // the sky-view caching above.
        VkImageView depth_view = ctx.target->depth_view();
        VkImageView normal_view = ctx.target->normal_view();
        if (depth_view != imgs.last_depth_view || normal_view != imgs.last_normal_view) {
            VkDescriptorImageInfo s_depth{};
            s_depth.sampler = linear_sampler_;
            s_depth.imageView = depth_view;
            s_depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

            VkDescriptorImageInfo s_normal{};
            s_normal.sampler = linear_sampler_;
            s_normal.imageView = normal_view;
            s_normal.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            std::array<VkWriteDescriptorSet, 2> sw{};
            for (auto& w : sw) { w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = imgs.sample_set; w.descriptorCount = 1; }
            sw[0].dstBinding = 1; sw[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sw[0].pImageInfo = &s_depth;
            sw[1].dstBinding = 2; sw[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sw[1].pImageInfo = &s_normal;

            vkUpdateDescriptorSets(vk_device_, static_cast<uint32_t>(sw.size()), sw.data(), 0, nullptr);
            imgs.last_depth_view = depth_view;
            imgs.last_normal_view = normal_view;
        }

        uint32_t half_res_val = config_.half_res_output ? 1u : 0u;
        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sample_pipeline_);
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                sample_pipeline_layout_, 0, 1, &imgs.sample_set, 0, nullptr);
        vkCmdPushConstants(ctx.cmd, sample_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(uint32_t), &half_res_val);
        vkCmdDispatch(ctx.cmd,
                      (imgs.indirect_extent.width + 7) / 8,
                      (imgs.indirect_extent.height + 7) / 8, 1);
    }

    // ── 12. Barrier: indirect image → shader read for fragment sampling ──
    barrier_image(imgs.indirect_image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_2_SHADER_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT);

    if (is_first_frame_) {
        FJELL_GFX_DEBUG("DDGI: First frame dispatched ({} probes, {} rays, indirect {}x{})",
                        imgs.total_probes, config_.probe_rays,
                        imgs.indirect_extent.width, imgs.indirect_extent.height);
    }
    is_first_frame_ = false;
    frame_counter_++;
}

PassSetup DdgiPass::setup() const {
    return {
        .name = "ddgi",
        .phase = RenderPhase::Color,
        .sub_order = 5,
    };
}

void DdgiPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    auto depth = builder.import_named(ctx, "depth");
    auto normal = builder.import_named(ctx, "normal");
    builder.read(depth, ResourceAccess::sampled_compute);
    builder.read(normal, ResourceAccess::sampled_compute);
    // Writes pass-private probe atlases (irradiance, visibility,
    // probe_offset) bound through the global descriptor. Graph can't
    // see that consumer edge.
    builder.has_side_effects();
}

} // namespace fjell
