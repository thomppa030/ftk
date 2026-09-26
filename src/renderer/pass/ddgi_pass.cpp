#include "renderer/pass/ddgi_pass.hpp"
#include "renderer/components/terrain_component.hpp"
#include "renderer/resources/terrain_asset.hpp"
#include "core/profiler.hpp"
#include "renderer/pass/pass_gpu_zone.hpp"
#include "renderer/gpu/accel_structure.hpp"
#include "renderer/gpu_context.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/post_process_resources.hpp"
#include "renderer/resource_registry.hpp"
#include "renderer/render_queue.hpp"
#include "renderer/render_stats.hpp"
#include "renderer/resources/model.hpp"
#include "renderer/systems/particle_system.hpp"
#include "renderer/gpu/vma_image.hpp"
#include "renderer/indirect_batch.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/resources/shader_utils.hpp"
#include "renderer/pass/scene_sdf_pass.hpp"
#include "environment/environment_state.hpp"
#include "scene/scene.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#include <imgui_impl_vulkan.h>

#include <glm/gtc/random.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>

namespace fjell {

// ── Images ──────────────────────────────────────────────────────────────

static void compute_atlas_dims(const DdgiConfig& config,
                                int32_t& irr_w, int32_t& irr_h,
                                int32_t& vis_w, int32_t& vis_h,
                                int32_t& total) {
    total = config.probe_counts.x * config.probe_counts.y * config.probe_counts.z;
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

    // Radiance: one row per probe traced THIS FRAME, not per probe in the
    // pool. The trace writes to `probe_index - probe_update_offset`, so
    // only the round-robin batch is ever addressed. Sizing this by the
    // pool wasted most of the image and put a hard ceiling on probe count:
    // at 32^3 the height would be 32768, twice the 16384 a device is
    // guaranteed to allow, and image creation would simply fail.
    radiance_rows = (config.probes_per_frame_batch > 0)
        ? std::min(config.probes_per_frame_batch, total_probes)
        : total_probes;
    create_image(radiance_image, radiance_memory, radiance_view,
                 static_cast<uint32_t>(config.probe_rays),
                 static_cast<uint32_t>(radiance_rows),
                 VK_FORMAT_R16G16B16A16_SFLOAT, storage_sampled);

    // Irradiance atlas
    create_image(irradiance_image, irradiance_memory, irradiance_view,
                 static_cast<uint32_t>(irr_width),
                 static_cast<uint32_t>(irr_height),
                 VK_FORMAT_R16G16B16A16_SFLOAT, storage_sampled);

    // Filter destination. The spatial filter reads every neighbour of a
    // probe, so it cannot write in place — a probe filtered early would
    // feed its filtered value to the ones after it. Sampling reads this
    // one; the update pass keeps writing the unfiltered atlas above.
    create_image(irradiance_filtered_image, irradiance_filtered_memory,
                 irradiance_filtered_view,
                 static_cast<uint32_t>(irr_width),
                 static_cast<uint32_t>(irr_height),
                 VK_FORMAT_R16G16B16A16_SFLOAT, storage_sampled);

    // Visibility atlas
    create_image(visibility_image, visibility_memory, visibility_view,
                 static_cast<uint32_t>(vis_width),
                 static_cast<uint32_t>(vis_height),
                 VK_FORMAT_R16G16_SFLOAT, storage_sampled);

    // Probe offsets: total_probes stored as 2D (probes_xy x probes_z)
    int probes_xy = config.probe_counts.x * config.probe_counts.y;
    create_image(probe_offset_image, probe_offset_memory, probe_offset_view,
                 static_cast<uint32_t>(probes_xy),
                 static_cast<uint32_t>(config.probe_counts.z),
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
        info.extent = {static_cast<uint32_t>(config.probe_counts.x),
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
    VkDeviceSize free_list_size = sizeof(uint32_t) * (1 + static_cast<VkDeviceSize>(total_probes));
    free_list_buffer = std::make_unique<Buffer>(
        allocator, free_list_size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO);

    // Probe status SSBO
    VkDeviceSize status_size = static_cast<VkDeviceSize>(total_probes) * sizeof(uint32_t);
    probe_status_buffer = std::make_unique<Buffer>(
        allocator, status_size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO);

    // Constants UBO, one per frame in flight
    for (auto& ubo : constants_ubo) {
        ubo = std::make_unique<Buffer>(
            allocator, sizeof(GpuDdgiConstants),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
        ubo->map();
    }

    // Indirect output (screen-res or half-res)
    uint32_t out_w = config.half_res_output ? std::max(1u, screen_w / 2) : screen_w;
    uint32_t out_h = config.half_res_output ? std::max(1u, screen_h / 2) : screen_h;
    indirect_extent = {out_w, out_h};

    create_image(indirect_image, indirect_memory, indirect_view,
                 out_w, out_h,
                 VK_FORMAT_R16G16B16A16_SFLOAT, storage_sampled);

    images_need_init = true;
    indirect_needs_init = true;
    // Every atlas is new and in UNDEFINED, whatever these said about the
    // images they replace: the next record has to take them all out of it.
    // A screen resize arriving before that record recreates the indirect
    // image too, and must not turn this into a resize-only init.
    first_frame_ = true;
    resized_only_ = false;
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
        trace_set.fill(VK_NULL_HANDLE);
        rt_trace_set.fill(VK_NULL_HANDLE);
        update_set.fill(VK_NULL_HANDLE);
        sample_set.fill(VK_NULL_HANDLE);
        filter_set.fill(VK_NULL_HANDLE);
    }

    destroy_img(radiance_view, radiance_image, radiance_memory);
    destroy_img(irradiance_view, irradiance_image, irradiance_memory);
    destroy_img(irradiance_filtered_view, irradiance_filtered_image, irradiance_filtered_memory);
    destroy_img(visibility_view, visibility_image, visibility_memory);
    destroy_img(probe_offset_view, probe_offset_image, probe_offset_memory);
    destroy_img(indirect_view, indirect_image, indirect_memory);
    for (int i = 0; i < 2; ++i) {
        destroy_img(indirection_view[i], indirection_image[i], indirection_memory[i]);
    }

    slot_grid_buffer.reset();
    free_list_buffer.reset();
    probe_status_buffer.reset();
    for (auto& ubo : constants_ubo) ubo.reset();
}

void DdgiPass::Images::resize(VkDevice device, VmaAllocator allocator,
                               const DdgiConfig& config,
                               uint32_t screen_w, uint32_t screen_h) {
    // First call: create everything
    if (radiance_image == VK_NULL_HANDLE) {
        create(device, allocator, config, screen_w, screen_h);
        return;
    }

    // Probe count drives the size of every probe atlas, so a change to it
    // means rebuilding all of them. Everything accumulated is discarded —
    // a probe's irradiance belongs to a grid position that no longer
    // exists once the grid is re-divided.
    int wanted_probes = config.probe_counts.x * config.probe_counts.y * config.probe_counts.z;
    if (wanted_probes != total_probes) {
        // Unlike a swapchain resize, this is driven by a settings change, so
        // frames referencing these images may still be in flight.
        vkDeviceWaitIdle(device);
        destroy(device, allocator);
        create(device, allocator, config, screen_w, screen_h);
        return;
    }

    // Subsequent calls: only recreate indirect_image if screen size changed
    uint32_t new_w = config.half_res_output ? std::max(1u, screen_w / 2) : screen_w;
    uint32_t new_h = config.half_res_output ? std::max(1u, screen_h / 2) : screen_h;
    if (indirect_extent.width == new_w && indirect_extent.height == new_h) return;

    // Only destroy/recreate the indirect output image, not the probe images
    vkDeviceWaitIdle(device);
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

    // Descriptors reference indirect_view, so they need rewriting. The probe
    // atlases were not touched: only this image is back in UNDEFINED, and
    // only its barrier may treat it that way.
    images_need_init = true;
    indirect_needs_init = true;
    resized_only_ = true;
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
    std::array<VkDescriptorSetLayoutBinding, 6> bindings{};

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

    // ── Spatial filter descriptor set layout ──
    // binding 0: DDGIConstants UBO
    // binding 1: source irradiance (sampler2D)
    // binding 2: visibility atlas (sampler2D) — the occlusion test
    // binding 3: destination irradiance (storage, write)
    // binding 5: probe status SSBO
    std::array<VkDescriptorSetLayoutBinding, 5> filter_bindings{};
    filter_bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    filter_bindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    filter_bindings[2] = {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    filter_bindings[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    filter_bindings[4] = {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

    VkDescriptorSetLayoutCreateInfo filter_layout_ci{};
    filter_layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    filter_layout_ci.bindingCount = static_cast<uint32_t>(filter_bindings.size());
    filter_layout_ci.pBindings = filter_bindings.data();

    if (vkCreateDescriptorSetLayout(vk_device_, &filter_layout_ci, nullptr, &filter_set_layout_) != VK_SUCCESS) {
        return make_error("Failed to create DDGI filter set layout");
    }

    VkPushConstantRange filter_push{};
    filter_push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    filter_push.offset = 0;
    filter_push.size = sizeof(FilterPushConstants);

    VkPipelineLayoutCreateInfo filter_pipe_layout_ci{};
    filter_pipe_layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    filter_pipe_layout_ci.setLayoutCount = 1;
    filter_pipe_layout_ci.pSetLayouts = &filter_set_layout_;
    filter_pipe_layout_ci.pushConstantRangeCount = 1;
    filter_pipe_layout_ci.pPushConstantRanges = &filter_push;

    if (vkCreatePipelineLayout(vk_device_, &filter_pipe_layout_ci, nullptr, &filter_pipeline_layout_) != VK_SUCCESS) {
        return make_error("Failed to create DDGI filter pipeline layout");
    }

    {
        auto code = read_spirv(base_path::resolve("shaders/ddgi_filter_irradiance.comp.spv"));
        VkShaderModule mod = create_shader_module(vk_device_, code);
        VkPipelineShaderStageCreateInfo st{};
        st.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        st.module = mod;
        st.pName = "main";
        VkComputePipelineCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ci.stage = st;
        ci.layout = filter_pipeline_layout_;
        VkResult r = vkCreateComputePipelines(vk_device_, resources.pipeline_cache, 1, &ci, nullptr, &irradiance_filter_pipeline_);
        vkDestroyShaderModule(vk_device_, mod, nullptr);
        if (r != VK_SUCCESS) {
            return make_error("Failed to create DDGI irradiance filter pipeline");
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
    std::array<VkDescriptorSetLayoutBinding, 8> sample_bindings{};
    sample_bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    sample_bindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    sample_bindings[2] = {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    sample_bindings[3] = {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    sample_bindings[4] = {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    sample_bindings[5] = {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    sample_bindings[6] = {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    sample_bindings[7] = {7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

    // Enable UPDATE_AFTER_BIND on sampler bindings (1,2 = depth, normal) so they
    // can be rewritten per-frame while the set is referenced by pending command buffers
    std::array<VkDescriptorBindingFlags, 8> sample_binding_flags{};
    sample_binding_flags[1] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; // depth
    sample_binding_flags[2] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; // normal
    // Irradiance: re-pointed when the spatial filter is toggled, which can
    // land while an earlier frame still references this set.
    sample_binding_flags[3] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;

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
        std::array<VkDescriptorSetLayoutBinding, 12> rt_bindings{};
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

        // Enable UPDATE_AFTER_BIND on bindings updated every frame
        std::array<VkDescriptorBindingFlags, 12> rt_bind_flags{};
        rt_bind_flags[1] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; // TLAS
        rt_bind_flags[6] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; // vertex buffer
        rt_bind_flags[7] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; // index buffer
        rt_bind_flags[8] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; // per-instance data
        rt_bind_flags[9] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; // irradiance (filter toggle)

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
    safe_destroy(vkDestroyPipeline, irradiance_filter_pipeline_);
    safe_destroy(vkDestroyPipeline, rt_pipeline_);
    safe_destroy(vkDestroyPipelineLayout, trace_pipeline_layout_);
    safe_destroy(vkDestroyPipelineLayout, update_pipeline_layout_);
    safe_destroy(vkDestroyPipelineLayout, sample_pipeline_layout_);
    safe_destroy(vkDestroyPipelineLayout, filter_pipeline_layout_);
    safe_destroy(vkDestroyPipelineLayout, rt_trace_pipeline_layout_);
    safe_destroy(vkDestroyDescriptorSetLayout, trace_set_layout_);
    safe_destroy(vkDestroyDescriptorSetLayout, update_set_layout_);
    safe_destroy(vkDestroyDescriptorSetLayout, sample_set_layout_);
    safe_destroy(vkDestroyDescriptorSetLayout, filter_set_layout_);
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
    if (!record_into(ctx, *imgs) && imgs->indirect_image != VK_NULL_HANDLE) {
        // Stopped before sampling. The graph moved the indirect output to
        // GENERAL for the write declare() promised; it goes back to where
        // the surfaces read it, holding what it held.
        transition_image(ctx.cmd, imgs->indirect_image,
                         VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
    imgs->indirect_needs_init = false;
}

static void write_trace_descriptors(VkDevice device, VkDescriptorSet set, uint32_t slot,
                                     DdgiPass::Images& imgs,
                                     VkImageView sdf_view, VkSampler sdf_sampler,
                                     VkImageView sky_view, VkSampler sky_sampler,
                                     VkSampler linear_sampler) {
    // 0: constants UBO
    VkDescriptorBufferInfo ubo_info{};
    ubo_info.buffer = imgs.constants_ubo[slot]->handle();
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

    std::array<VkWriteDescriptorSet, 6> writes{};
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

    vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
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
    constants.irr_tex_w = imgs.irr_width;
    constants.irr_tex_h = imgs.irr_height;
    constants.irr_side = config_.irradiance_side_length;
    constants.probe_rays = config_.probe_rays;
    constants.vis_tex_w = imgs.vis_width;
    constants.vis_tex_h = imgs.vis_height;
    constants.vis_side = config_.visibility_side_length;

    // Round-robin a slice of probes each frame to cut per-frame ray cost.
    //
    // The count can never exceed the radiance image's height: the trace
    // writes one row per probe at `probe_index - probe_update_offset`, so a
    // wider window writes past the end of the image and the device is lost.
    // That is a real hazard now the image is sized to the batch rather than
    // to the whole pool, and it is not only a first-frame concern — a grid
    // re-fit takes the same path.
    int total = imgs.total_probes;
    int max_rows = (imgs.radiance_rows > 0) ? imgs.radiance_rows : total;
    int batch = std::min(config_.probes_per_frame_batch, max_rows);
    bool resetting = imgs.first_frame_ || imgs.reset_generation_ != grid_generation_;
    if (batch <= 0 || batch >= total) {
        constants.probe_update_offset = 0;
        constants.probe_update_count = std::min(total, max_rows);
    } else if (resetting) {
        // A reset cannot refresh the whole pool in one frame any more, so
        // it restarts the sweep instead and lets the round-robin catch up.
        // Every probe is UNINITIALIZED, so each is overwritten rather than
        // blended as the cursor reaches it.
        imgs.probe_update_offset = 0;
        constants.probe_update_offset = 0;
        constants.probe_update_count = batch;
        imgs.probe_update_offset = batch;
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

    std::memcpy(imgs.constants_ubo[ctx.frame_index]->mapped(), &constants, sizeof(constants));
}

void DdgiPass::prepare_frame(const FrameContext& ctx, GpuContext& gpu) {
    if (!ctx.scene) return;

    // Terrain's render geometry is generated in a mesh shader and never exists
    // as triangles, so ray tracing needs the decimated mesh the asset builds
    // from its height array. Done here rather than in pre_record because this
    // is the hook that carries the scene.
    if (backend_ == DdgiBackend::ray_traced && accel_manager_ != nullptr &&
        ctx.mega_buffer != nullptr) {
        for (auto [obj, tc] : ctx.scene->view<TerrainComponent>()) {
            if (tc->terrain == nullptr) continue;
            tc->terrain->upload_ray_mesh(*ctx.mega_buffer);
            if (!tc->terrain->ray_mesh_uploaded()) continue;
            // A sculpt moves the ground the rays trace against. The mesh keeps
            // its place in the buffer — same grid, new heights — so the BLAS
            // is still valid memory describing triangles that have moved, and
            // has to be rebuilt over them.
            const bool moved = tc->terrain->refresh_ray_mesh(*ctx.mega_buffer);
            if (moved) {
                // The new vertices are a batched copy that would not be
                // submitted until the frame ends, while the build below
                // submits and waits on its own queue — it would read the
                // heights the sculpt replaced.
                gpu.upload_context().wait_all();
            }
            accel_manager_->ensure_blas_entry(tc->terrain, tc->terrain->ray_mesh_entry(),
                                              *ctx.mega_buffer, moved);
        }
    }

    if (ctx.environment != nullptr) {
        const auto& sun = ctx.environment->primary;
        light_direction_ = sun.direction;
        light_intensity_ = sun.intensity;
        light_color_ = sun.color;
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
    glm::vec3 grid_min = config_.grid_position;
    glm::vec3 grid_max = config_.grid_position + glm::vec3(config_.probe_counts - glm::ivec3(1)) * config_.probe_spacing;
    return {grid_min - glm::vec3(MAX_TRACE_DIST), grid_max + glm::vec3(MAX_TRACE_DIST)};
}

void DdgiPass::prepare_accel_structures(const FrameContext& ctx) {
    if (backend_ != DdgiBackend::ray_traced || !accel_manager_ || !ctx.queue || !ctx.mega_buffer)
        return;

    // The reach every tracer needs, not just this pass's own. A model inside
    // reflection range but outside the probe grid's still has to be built, or
    // the TLAS below adds an instance whose BLAS was never made and the
    // reflection ray passes through it.
    accel_manager_->register_trace_reach(trace_reach_bounds());
    AABB bounds = accel_manager_->required_reach();
    if (!bounds.valid()) return;

    // Off-screen geometry needs a BLAS as much as visible geometry: the TLAS
    // build skips any model without one, so an object that has been off
    // screen since load would otherwise never make it in.
    auto ensure = [&](const DrawCommand& dc) {
        if (!dc.model) return;
        if (!dc.world_aabb.intersects(bounds)) return;
        accel_manager_->ensure_blas(dc.model, *ctx.mega_buffer);
    };
    for (const auto& dc : ctx.queue->commands()) ensure(dc);
    for (const auto& dc : ctx.queue->gi_only_commands()) ensure(dc);

    // Cache particle SSBO for compute dispatch + build particle mesh BLASes
    if (particle_system_) {
        VkBuffer pbuf = particle_system_->particle_buffer_handle();
        if (pbuf != VK_NULL_HANDLE) {
            accel_manager_->set_particle_ssbo(pbuf);
        }

        for (const auto& em : particle_system_->emitters()) {
            if (!em.is_mesh || !em.in_scene) continue;
            auto shape_key = reinterpret_cast<const void*>(static_cast<uintptr_t>(em.mesh_entry.first_index + 1));
            accel_manager_->ensure_blas_entry(shape_key, em.mesh_entry, *ctx.mega_buffer);
        }
    }
}

bool DdgiPass::record_into(const FrameContext& ctx, Images& imgs) {
    if (trace_pipeline_ == VK_NULL_HANDLE) return false;
    if (imgs.radiance_image == VK_NULL_HANDLE) return false;
    if (ctx.registry == nullptr) return false;

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
        // Rewriting the descriptor only points the trace at the new sky; the
        // atlases still hold irradiance integrated from the old one. An HDRI
        // finishing its async load a few frames into a scene would otherwise
        // leave every probe lit by the placeholder sky until something else
        // invalidated the grid.
        if (imgs.last_sky_view != VK_NULL_HANDLE) invalidate_probe_lighting();
        imgs.last_sky_view = sky_cubemap_view;
        imgs.last_sky_sampler = sky_cubemap_sampler;
    }

    // Allocate descriptor pool + sets for this Images instance if needed
    if (imgs.desc_pool == VK_NULL_HANDLE) {
        bool has_rt = rt_trace_set_layout_ != VK_NULL_HANDLE;
        // Four sets per frame slot without RT (trace, update, sample,
        // filter). With RT, one RT trace set per slot on top; each adds
        // 1 UBO, 5 samplers, 1 storage image, 4 storage buffers and 1
        // acceleration structure.
        constexpr uint32_t SLOTS = MAX_FRAMES_IN_FLIGHT;
        constexpr uint32_t RT_SLOTS = MAX_FRAMES_IN_FLIGHT;
        uint32_t max_sets = has_rt ? 4u * SLOTS + RT_SLOTS : 4u * SLOTS;

        std::array<VkDescriptorPoolSize, 5> pool_sizes{};
        pool_sizes[0] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, has_rt ? 4u * SLOTS + RT_SLOTS : 4u * SLOTS};
        pool_sizes[1] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, has_rt ? 11u * SLOTS + 5u * RT_SLOTS : 11u * SLOTS};
        pool_sizes[2] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, has_rt ? 6u * SLOTS + RT_SLOTS : 6u * SLOTS};
        pool_sizes[3] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, has_rt ? 4u * SLOTS + 4u * RT_SLOTS : 4u * SLOTS};
        pool_sizes[4] = {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, has_rt ? RT_SLOTS : 0u};
        uint32_t pool_size_count = has_rt ? 5u : 4u;

        VkDescriptorPoolCreateInfo pool_ci{};
        pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_ci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        pool_ci.maxSets = max_sets;
        pool_ci.poolSizeCount = pool_size_count;
        pool_ci.pPoolSizes = pool_sizes.data();

        vkCreateDescriptorPool(vk_device_, &pool_ci, nullptr, &imgs.desc_pool);

        // Slot s owns layouts[4s..4s+3]; the RT sets follow.
        std::array<VkDescriptorSetLayout, 4 * SLOTS + RT_SLOTS> layouts{};
        for (uint32_t s = 0; s < SLOTS; ++s) {
            layouts[4 * s + 0] = trace_set_layout_;
            layouts[4 * s + 1] = update_set_layout_;
            layouts[4 * s + 2] = sample_set_layout_;
            layouts[4 * s + 3] = filter_set_layout_;
        }
        for (uint32_t s = 0; s < RT_SLOTS; ++s) layouts[4 * SLOTS + s] = rt_trace_set_layout_;
        std::array<VkDescriptorSet, 4 * SLOTS + RT_SLOTS> sets{};

        VkDescriptorSetAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = imgs.desc_pool;
        alloc_info.descriptorSetCount = max_sets;
        alloc_info.pSetLayouts = layouts.data();

        vkAllocateDescriptorSets(vk_device_, &alloc_info, sets.data());
        for (uint32_t s = 0; s < SLOTS; ++s) {
            imgs.trace_set[s] = sets[4 * s + 0];
            imgs.update_set[s] = sets[4 * s + 1];
            imgs.sample_set[s] = sets[4 * s + 2];
            imgs.filter_set[s] = sets[4 * s + 3];
        }
        if (has_rt) {
            for (uint32_t s = 0; s < RT_SLOTS; ++s) imgs.rt_trace_set[s] = sets[4 * SLOTS + s];
        }
        imgs.images_need_init = true;
    }

    // Update constants UBO
    update_constants(imgs, ctx);

    // Write descriptors on first use or when views become available
    if (imgs.images_need_init) {
        if (sdf_view == VK_NULL_HANDLE) {
            FJELL_GFX_WARN("DDGI: SDF view not available, skipping");
            frame_counter_++;
            return false;
        }
        if (sky_cubemap_view == VK_NULL_HANDLE) {
            FJELL_GFX_WARN("DDGI: Sky cubemap view not available, skipping");
            frame_counter_++;
            return false;
        }
        // FJELL_GFX_DEBUG("DDGI: Writing descriptors (first init)");
    }
    if (imgs.images_need_init && sdf_view != VK_NULL_HANDLE && sky_cubemap_view != VK_NULL_HANDLE) {
        for (uint32_t s = 0; s < MAX_FRAMES_IN_FLIGHT; ++s) {
            write_trace_descriptors(vk_device_, imgs.trace_set[s], s, imgs,
                                    sdf_view, sdf_sampler,
                                    sky_cubemap_view, sky_cubemap_sampler,
                                    linear_sampler_);
        }

        // Write update set descriptors, the same into every slot's set but
        // for that slot's UBO
        VkDescriptorBufferInfo ubo_info{};
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

        for (uint32_t s = 0; s < MAX_FRAMES_IN_FLIGHT; ++s) {
            ubo_info.buffer = imgs.constants_ubo[s]->handle();
            std::array<VkWriteDescriptorSet, 6> writes{};
            for (auto& w : writes) {
                w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w.dstSet = imgs.update_set[s];
                w.descriptorCount = 1;
            }
            writes[0].dstBinding = 0; writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; writes[0].pBufferInfo = &ubo_info;
            writes[1].dstBinding = 1; writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[1].pImageInfo = &radiance_info;
            writes[2].dstBinding = 2; writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; writes[2].pImageInfo = &irr_info;
            writes[3].dstBinding = 3; writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; writes[3].pImageInfo = &vis_info;
            writes[4].dstBinding = 4; writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; writes[4].pImageInfo = &offset_info;
            writes[5].dstBinding = 5; writes[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[5].pBufferInfo = &status_info;

            vkUpdateDescriptorSets(vk_device_, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }

        // Write sample set static descriptors (irradiance, visibility, offset, status, indirect)
        // Depth + normal are written every frame since they change with viewport resize
        {
            VkDescriptorBufferInfo s_ubo{};
            s_ubo.range = sizeof(GpuDdgiConstants);

            // Consumers read the FILTERED atlas; the update pass keeps
            // writing the raw one, which the filter reads from.
            VkDescriptorImageInfo s_irr{};
            s_irr.sampler = linear_sampler_;
            s_irr.imageView = config_.spatial_filter ? imgs.irradiance_filtered_view
                                                     : imgs.irradiance_view;
            imgs.bound_to_filtered = config_.spatial_filter;
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

            for (uint32_t s = 0; s < MAX_FRAMES_IN_FLIGHT; ++s) {
                s_ubo.buffer = imgs.constants_ubo[s]->handle();
                std::array<VkWriteDescriptorSet, 6> sw{};
                for (auto& w : sw) { w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = imgs.sample_set[s]; w.descriptorCount = 1; }
                sw[0].dstBinding = 0; sw[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; sw[0].pBufferInfo = &s_ubo;
                sw[1].dstBinding = 3; sw[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sw[1].pImageInfo = &s_irr;
                sw[2].dstBinding = 4; sw[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sw[2].pImageInfo = &s_vis;
                sw[3].dstBinding = 5; sw[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sw[3].pImageInfo = &s_offset;
                sw[4].dstBinding = 6; sw[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; sw[4].pBufferInfo = &s_status;
                sw[5].dstBinding = 7; sw[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; sw[5].pImageInfo = &s_indirect;

                vkUpdateDescriptorSets(vk_device_, static_cast<uint32_t>(sw.size()), sw.data(), 0, nullptr);
            }
        }

        // Write RT trace descriptors (static bindings only — TLAS updated
        // per-frame), the same into every slot's set.
        if (imgs.rt_trace_set[0] != VK_NULL_HANDLE) {
            VkDescriptorBufferInfo rt_ubo{};
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
            rt_irradiance.imageView = config_.spatial_filter ? imgs.irradiance_filtered_view
                                                             : imgs.irradiance_view;
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

            // Write bindings 0, 2-5, 9-11 (skip 1=TLAS, 6-8=buffers, updated per-frame)
            for (uint32_t s = 0; s < MAX_FRAMES_IN_FLIGHT; ++s) {
            VkDescriptorSet rt_set = imgs.rt_trace_set[s];
            rt_ubo.buffer = imgs.constants_ubo[s]->handle();
            std::array<VkWriteDescriptorSet, 8> rtw{};
            for (auto& w : rtw) { w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = rt_set; w.descriptorCount = 1; }
            rtw[0].dstBinding = 0; rtw[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; rtw[0].pBufferInfo = &rt_ubo;
            rtw[1].dstBinding = 2; rtw[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; rtw[1].pImageInfo = &rt_radiance;
            rtw[2].dstBinding = 3; rtw[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; rtw[2].pImageInfo = &rt_sky;
            rtw[3].dstBinding = 4; rtw[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; rtw[3].pBufferInfo = &rt_status;
            rtw[4].dstBinding = 5; rtw[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; rtw[4].pImageInfo = &rt_offset;
            rtw[5].dstBinding = 9; rtw[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; rtw[5].pImageInfo = &rt_irradiance;
            rtw[6].dstBinding = 10; rtw[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; rtw[6].pImageInfo = &rt_visibility;
            rtw[7].dstBinding = 11; rtw[7].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; rtw[7].pImageInfo = &rt_sdf;

            vkUpdateDescriptorSets(vk_device_, static_cast<uint32_t>(rtw.size()), rtw.data(), 0, nullptr);
            }
        }

        // Spatial filter set. Fixed for the life of an Images: the filter
        // always reads the update pass's atlas and writes the filtered one.
        {
            VkDescriptorBufferInfo f_ubo{};
            f_ubo.range = sizeof(GpuDdgiConstants);

            VkDescriptorImageInfo f_src{};
            f_src.sampler = linear_sampler_;
            f_src.imageView = imgs.irradiance_view;
            f_src.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            VkDescriptorImageInfo f_vis{};
            f_vis.sampler = linear_sampler_;
            f_vis.imageView = imgs.visibility_view;
            f_vis.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            VkDescriptorImageInfo f_dst{};
            f_dst.imageView = imgs.irradiance_filtered_view;
            f_dst.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            VkDescriptorBufferInfo f_status{};
            f_status.buffer = imgs.probe_status_buffer->handle();
            f_status.range = VK_WHOLE_SIZE;

            for (uint32_t s = 0; s < MAX_FRAMES_IN_FLIGHT; ++s) {
                f_ubo.buffer = imgs.constants_ubo[s]->handle();
                std::array<VkWriteDescriptorSet, 5> fw{};
                for (auto& w : fw) { w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = imgs.filter_set[s]; w.descriptorCount = 1; }
                fw[0].dstBinding = 0; fw[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; fw[0].pBufferInfo = &f_ubo;
                fw[1].dstBinding = 1; fw[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; fw[1].pImageInfo = &f_src;
                fw[2].dstBinding = 2; fw[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; fw[2].pImageInfo = &f_vis;
                fw[3].dstBinding = 3; fw[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; fw[3].pImageInfo = &f_dst;
                fw[4].dstBinding = 5; fw[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; fw[4].pBufferInfo = &f_status;

                vkUpdateDescriptorSets(vk_device_, static_cast<uint32_t>(fw.size()), fw.data(), 0, nullptr);
            }
        }

        // A rewrite triggered by a screen resize leaves the atlases intact —
        // only a fresh create() has no probe data to preserve.
        if (!imgs.resized_only_) imgs.first_frame_ = true;
        imgs.resized_only_ = false;
        imgs.images_need_init = false;
    }

    // SDF backend requires the SDF view
    if (backend_ == DdgiBackend::sdf_fallback && sdf_view == VK_NULL_HANDLE) {
        frame_counter_++;
        return false;
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

    GpuDdgiConstants* constants =
        static_cast<GpuDdgiConstants*>(imgs.constants_ubo[ctx.frame_index]->mapped());

    // ── 0. Initialize probe status buffer on first use ──
    bool resetting_atlases = imgs.first_frame_ || imgs.reset_generation_ != grid_generation_;
    if (resetting_atlases) {
        // Every probe starts without history: the irradiance atlas still
        // holds whatever the previous grid left there, which belongs to
        // different world positions. The update pass overwrites rather than
        // blends while this flag is set, and clears it per probe as the
        // round-robin sweep reaches each one.
        constexpr uint32_t PROBE_STATUS_UNINITIALIZED = 6;
        VkDeviceSize status_size = static_cast<VkDeviceSize>(imgs.total_probes) * sizeof(uint32_t);
        vkCmdFillBuffer(ctx.cmd, imgs.probe_status_buffer->handle(), 0, status_size,
                        PROBE_STATUS_UNINITIALIZED);

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

    // ── 1. Pre-trace barriers ──
    barrier_image(imgs.radiance_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                  VK_ACCESS_2_SHADER_READ_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
    barrier_image(imgs.probe_offset_image,
                  imgs.first_frame_ ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_2_SHADER_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT);

    // Irradiance + visibility must be in GENERAL before the trace reads them (multi-bounce)
    if (imgs.first_frame_) {
        barrier_image(imgs.irradiance_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                      0, VK_ACCESS_2_SHADER_READ_BIT);
        barrier_image(imgs.visibility_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                      0, VK_ACCESS_2_SHADER_READ_BIT);
        // The sample and RT sets bind the filtered atlas as GENERAL when
        // they are written at init, but the filter pass — which is what
        // would otherwise transition it — does not run until later in the
        // frame. Move it here so the descriptor's promise holds from the
        // first submit.
        barrier_image(imgs.irradiance_filtered_image, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_2_SHADER_READ_BIT);
    }

    // ── 2. Build TLAS + dispatch trace (RT or SDF) ──
    // Build TLAS if RT backend is active (BLAS building done in prepare_accel_structures)
    if (backend_ == DdgiBackend::ray_traced && accel_manager_ && ctx.queue && ctx.mega_buffer) {
        FJELL_PROFILE_SCOPE_N("ddgi_tlas_build");
        FJELL_GPU_ZONE(ctx, "DDGI TLAS Build");
        const auto& cmds = ctx.queue->commands();
        {
            // Only include objects some tracer can reach. The union of every
            // pass's declared reach, not this pass's alone: the TLAS is shared
            // and built once, so anything left out here is missing for
            // reflections and screen-space GI too, not only for the probes.
            //
            // Registered again here rather than relying on the BLAS pass
            // having done it: that one is skipped for a viewport with DDGI
            // off, while this still builds the TLAS, and the union would then
            // be short exactly this pass's own reach. Registering is a union,
            // so doing it twice costs nothing.
            accel_manager_->register_trace_reach(trace_reach_bounds());
            AABB bounds = accel_manager_->required_reach();

            // Everything whose instance count is known on the CPU goes in one
            // list; build_tlas applies the same transform validation to all of
            // it and reserves the particle region after.
            // Visible draws plus the opaque ones the camera culled: those
            // are off screen but still light, and shadow, what is on it.
            const auto& gi_only = ctx.queue->gi_only_commands();
            std::vector<AccelStructureManager::RtInstance> host_instances;
            host_instances.reserve(cmds.size() + gi_only.size() + 1);

            // One instance per submesh the command draws, each over that
            // submesh's own BLAS, so a hit reports the triangle against the
            // material and index range of the geometry it actually struck.
            auto add_instances = [&](const DrawCommand& dc) {
                if (!dc.model) return;
                if (!dc.world_aabb.intersects(bounds)) return;
                const auto& lods = dc.model->lod_levels();
                if (lods.empty()) return;
                auto blases = accel_manager_->model_blases(dc.model);
                for (uint32_t i = 0; i < blases.size(); ++i) {
                    if (!dc.model->entry_uses_slot(lods[0].first_mega_entry + i, dc.material_slot)) continue;

                    AccelStructureManager::RtInstance inst{};
                    inst.transform = dc.push.model;
                    inst.material_id = dc.push.material_id;
                    inst.blas = &blases[i];
                    host_instances.push_back(inst);
                }
            };
            for (const auto& dc : cmds) add_instances(dc);
            for (const auto& dc : gi_only) add_instances(dc);

            // Terrain generates its render geometry in a mesh shader, so it has
            // no draw to ride in on and is collected from the scene directly.
            if (ctx.scene != nullptr) {
                for (auto [obj, tc] : ctx.scene->view<TerrainComponent>()) {
                    if (tc->terrain == nullptr) continue;
                    const auto* blas = accel_manager_->find_blas(tc->terrain);
                    if (blas == nullptr) continue;

                    const auto* spatial = obj->as_spatial();
                    // Translation only, as the raster path and physics place
                    // it: the terrain's shape is its asset's, sampled along
                    // world X and Z, and the node only says where. Passing the
                    // full matrix here would rotate what rays hit away from
                    // what is drawn.
                    AccelStructureManager::RtInstance inst{};
                    inst.transform = glm::translate(
                        glm::mat4{1.0F},
                        spatial != nullptr ? glm::vec3{spatial->world_matrix_[3]}
                                           : glm::vec3{0.0F});
                    // Rays cannot run the layer blend, so the terrain bounces
                    // its first layer, which is the ground where nothing has
                    // been painted or sloped into another.
                    inst.material_id = tc->terrain->layer_material_id(0);
                    inst.blas = blas;
                    host_instances.push_back(inst);
                }
            }

            // Collect mesh particle emitters for per-particle TLAS instances
            std::vector<AccelStructureManager::ParticleEmitterInfo> particle_infos;
            if (particle_system_) {
                for (const auto& em : particle_system_->emitters()) {
                    if (!em.is_mesh || !em.in_scene) continue;
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

            if (!host_instances.empty() || !particle_infos.empty()) {
                accel_manager_->build_tlas(ctx.cmd, ctx.frame_index, host_instances,
                                           particle_infos);
            }
            if (ctx.render_stats != nullptr) {
                ctx.render_stats->tlas_instances += static_cast<uint32_t>(host_instances.size());
            }
        }
    }

    // Keep vertex/index buffer descriptors current (MegaBuffer may
    // reallocate) on this slot's set; a pending frame keeps the retired
    // buffer its own set still points at.
    if (imgs.rt_trace_set[ctx.frame_index] != VK_NULL_HANDLE && ctx.mega_buffer) {
        VkDescriptorBufferInfo verts_info{};
        verts_info.buffer = ctx.mega_buffer->vertex_buffer_handle();
        verts_info.range = VK_WHOLE_SIZE;

        VkDescriptorBufferInfo indices_info{};
        indices_info.buffer = ctx.mega_buffer->index_buffer_handle();
        indices_info.range = VK_WHOLE_SIZE;

        VkWriteDescriptorSet verts_write{};
        verts_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        verts_write.dstSet = imgs.rt_trace_set[ctx.frame_index];
        verts_write.dstBinding = 6;
        verts_write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        verts_write.descriptorCount = 1;
        verts_write.pBufferInfo = &verts_info;

        VkWriteDescriptorSet indices_write{};
        indices_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        indices_write.dstSet = imgs.rt_trace_set[ctx.frame_index];
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
                  && imgs.rt_trace_set[ctx.frame_index] != VK_NULL_HANDLE;

    if (use_rt) {
        FJELL_PROFILE_SCOPE_N("ddgi_trace_rt");
        FJELL_GPU_ZONE(ctx, "DDGI Trace RT");
        // Update TLAS descriptor (changes every frame) on this slot's set
        VkDescriptorSet rt_set = imgs.rt_trace_set[ctx.frame_index];
        VkWriteDescriptorSetAccelerationStructureKHR tlas_write_as{};
        tlas_write_as.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
        tlas_write_as.accelerationStructureCount = 1;
        VkAccelerationStructureKHR tlas = accel_manager_->tlas(ctx.frame_index);
        tlas_write_as.pAccelerationStructures = &tlas;

        VkWriteDescriptorSet tlas_write{};
        tlas_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        tlas_write.dstSet = rt_set;
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
        inst_write.dstSet = rt_set;
        inst_write.dstBinding = 8;
        inst_write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        inst_write.descriptorCount = 1;
        inst_write.pBufferInfo = &inst_data_info;

        std::array writes = {tlas_write, inst_write};
        vkUpdateDescriptorSets(vk_device_, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

        // Bind RT pipeline and descriptors
        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rt_pipeline_);
        std::array rt_sets = {rt_set, ctx.bindless_set};
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                                rt_trace_pipeline_layout_, 0,
                                static_cast<uint32_t>(rt_sets.size()), rt_sets.data(),
                                0, nullptr);

        RtTracePushConstants rt_push{};
        rt_push.light_direction = light_direction_;
        rt_push.light_intensity = light_intensity_;
        rt_push.light_color = light_color_;
        rt_push.max_trace_dist = MAX_TRACE_DIST;
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
                                trace_pipeline_layout_, 0, 1, &imgs.trace_set[ctx.frame_index], 0, nullptr);

        TracePushConstants push{};
        push.light_direction = light_direction_;
        push.light_intensity = light_intensity_;
        push.light_color = light_color_;
        push.sdf_resolution = static_cast<float>(SceneSdfPass::DEFAULT_RESOLUTION);
        push.sdf_volume_min = config_.grid_position;
        push.sdf_volume_max = config_.grid_position + glm::vec3(config_.probe_counts - glm::ivec3(1)) * config_.probe_spacing;

        vkCmdPushConstants(ctx.cmd, trace_pipeline_layout_,
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);

        uint32_t groups_x = (static_cast<uint32_t>(config_.probe_rays) + 31) / 32;
        uint32_t groups_y = static_cast<uint32_t>(constants->probe_update_count);
        vkCmdDispatch(ctx.cmd, groups_x, groups_y, 1);
    } else {
        // RT backend but TLAS not ready — skip trace this frame, keep previous
        // radiance and the previous indirect output.
        frame_counter_++;
        return false;
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
                  imgs.first_frame_ ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                  VK_IMAGE_LAYOUT_GENERAL,
                  VK_ACCESS_2_SHADER_READ_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
    barrier_image(imgs.visibility_image,
                  imgs.first_frame_ ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                  VK_IMAGE_LAYOUT_GENERAL,
                  VK_ACCESS_2_SHADER_READ_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);

    struct UpdatePush {
        uint32_t first_frame;
        float max_trace_dist;
    } update_push{resetting_atlases ? 1U : 0U, MAX_TRACE_DIST};

    // ── 5. Dispatch irradiance update ──
    {
        FJELL_PROFILE_SCOPE_N("ddgi_update_irradiance");
        FJELL_GPU_ZONE(ctx, "DDGI Update Irradiance");
        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, irradiance_update_pipeline_);
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                update_pipeline_layout_, 0, 1, &imgs.update_set[ctx.frame_index], 0, nullptr);
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
                                update_pipeline_layout_, 0, 1, &imgs.update_set[ctx.frame_index], 0, nullptr);
        vkCmdPushConstants(ctx.cmd, update_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(update_push), &update_push);
        vkCmdDispatch(ctx.cmd,
                      (static_cast<uint32_t>(imgs.vis_width) + 7) / 8,
                      (static_cast<uint32_t>(imgs.vis_height) + 7) / 8, 1);
    }

    // The sample and RT sets bind the irradiance atlas once at init, so a
    // runtime toggle of the filter has to re-point them; otherwise the
    // setting changes and nothing observable does.
    if (imgs.sample_set[0] != VK_NULL_HANDLE
        && imgs.bound_to_filtered != config_.spatial_filter) {
        VkDescriptorImageInfo irr{};
        irr.sampler = linear_sampler_;
        irr.imageView = config_.spatial_filter ? imgs.irradiance_filtered_view
                                               : imgs.irradiance_view;
        irr.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        // Every slot's sample set and RT trace set: this binding is static
        // per set, so all slots move together.
        std::array<VkWriteDescriptorSet, 2 * MAX_FRAMES_IN_FLIGHT> w{};
        uint32_t count = 0;
        for (VkDescriptorSet sample_set : imgs.sample_set) {
            w[count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[count].dstSet = sample_set;
            w[count].dstBinding = 3;
            w[count].descriptorCount = 1;
            w[count].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[count].pImageInfo = &irr;
            ++count;
        }

        for (VkDescriptorSet rt_set : imgs.rt_trace_set) {
            if (rt_set == VK_NULL_HANDLE) continue;
            w[count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[count].dstSet = rt_set;
            w[count].dstBinding = 9;
            w[count].descriptorCount = 1;
            w[count].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[count].pImageInfo = &irr;
            ++count;
        }

        vkUpdateDescriptorSets(vk_device_, count, w.data(), 0, nullptr);
        imgs.bound_to_filtered = config_.spatial_filter;
    }

    // ── 6.5 Spatial filter ──
    // Blends each probe with its face-adjacent neighbours so the trilinear
    // cage has less to disagree about. Runs after both update passes: it
    // reads this frame's irradiance and the visibility atlas the occlusion
    // test depends on.
    if (config_.spatial_filter) {
        FJELL_PROFILE_SCOPE_N("ddgi_filter_irradiance");
        FJELL_GPU_ZONE(ctx, "DDGI Filter Irradiance");

        barrier_memory();
        barrier_image(imgs.irradiance_filtered_image,
                      VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                      VK_ACCESS_2_SHADER_READ_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);

        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, irradiance_filter_pipeline_);
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                filter_pipeline_layout_, 0, 1, &imgs.filter_set[ctx.frame_index], 0, nullptr);

        uint32_t gx = (static_cast<uint32_t>(imgs.irr_width) + 7) / 8;
        uint32_t gy = (static_cast<uint32_t>(imgs.irr_height) + 7) / 8;

        // Interiors, then the border ring that mirrors them — the border
        // reads interior texels, so it cannot run in the same dispatch.
        for (uint32_t phase = 0; phase < 2; ++phase) {
            FilterPushConstants fpc{};
            fpc.max_hit_angle = config_.filter_max_hit_angle;
            fpc.border_phase = phase;
            vkCmdPushConstants(ctx.cmd, filter_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                               0, sizeof(fpc), &fpc);
            vkCmdDispatch(ctx.cmd, gx, gy, 1);
            if (phase == 0) barrier_memory();
        }
    }

    // The sample pass reads whichever atlas the consumers were bound to.
    if (config_.spatial_filter) {
        barrier_memory();
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
                                update_pipeline_layout_, 0, 1, &imgs.update_set[ctx.frame_index], 0, nullptr);
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
                                update_pipeline_layout_, 0, 1, &imgs.update_set[ctx.frame_index], 0, nullptr);
        vkCmdPushConstants(ctx.cmd, update_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(update_push), &update_push);
        vkCmdDispatch(ctx.cmd, (total + 31) / 32, 1, 1);
    }

    // ── 10. The graph has the indirect image in GENERAL for sample output ──

    // ── 11. Dispatch fullscreen sampling (skip first frame — depth buffer not yet rendered) ──
    if (!imgs.first_frame_) {
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

            for (VkDescriptorSet sample_set : imgs.sample_set) {
                std::array<VkWriteDescriptorSet, 2> sw{};
                for (auto& w : sw) { w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = sample_set; w.descriptorCount = 1; }
                sw[0].dstBinding = 1; sw[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sw[0].pImageInfo = &s_depth;
                sw[1].dstBinding = 2; sw[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sw[1].pImageInfo = &s_normal;

                vkUpdateDescriptorSets(vk_device_, static_cast<uint32_t>(sw.size()), sw.data(), 0, nullptr);
            }
            imgs.last_depth_view = depth_view;
            imgs.last_normal_view = normal_view;
        }

        uint32_t half_res_val = config_.half_res_output ? 1u : 0u;
        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sample_pipeline_);
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                sample_pipeline_layout_, 0, 1, &imgs.sample_set[ctx.frame_index], 0, nullptr);
        vkCmdPushConstants(ctx.cmd, sample_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(uint32_t), &half_res_val);
        vkCmdDispatch(ctx.cmd,
                      (imgs.indirect_extent.width + 7) / 8,
                      (imgs.indirect_extent.height + 7) / 8, 1);
    }

    // ── 12. Barrier: indirect image → shader read for fragment sampling ──
    barrier_image(imgs.indirect_image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_2_SHADER_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT);

    if (imgs.first_frame_) {
        FJELL_GFX_DEBUG("DDGI: First frame dispatched ({} probes, {} rays, indirect {}x{})",
                        imgs.total_probes, config_.probe_rays,
                        imgs.indirect_extent.width, imgs.indirect_extent.height);
    }
    imgs.first_frame_ = false;
    imgs.reset_generation_ = grid_generation_;
    frame_counter_++;
    return true;
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
    // The particle TLAS build reads the simulated particles.
    builder.read(builder.import_named_buffer(ctx, "particle_state"),
                 ResourceAccess::storage_buffer_read_compute);
    // Probe rays that miss read the sky, in the ray tracing stages or in the
    // SDF trace's compute shader.
    if (auto sky = builder.import_named(ctx, "sky_cubemap"); sky.valid()) {
        const bool traced = backend_ == DdgiBackend::ray_traced && rt_pipeline_ != VK_NULL_HANDLE;
        builder.read(sky, traced ? ResourceAccess::sampled_raytracing
                                 : ResourceAccess::sampled_compute);
    }
    // The per-pixel indirect light the surfaces sample. The graph moves it
    // into GENERAL; record() leaves it back where the surfaces read it on
    // every way out, which final_layout reports.
    if (config_.enabled) {
        if (auto out = builder.import_named_optional(ctx, "ddgi_indirect"); out.valid()) {
            builder.write(out, ResourceAccess::storage_write_compute);
            builder.final_layout(out, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                 VK_ACCESS_2_SHADER_WRITE_BIT,
                                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }
    }
    // Writes pass-private probe atlases (irradiance, visibility,
    // probe_offset) bound through the global descriptor. Graph can't
    // see that consumer edge.
    builder.has_side_effects();
}

} // namespace fjell
