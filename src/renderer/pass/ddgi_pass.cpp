#include "renderer/pass/ddgi_pass.hpp"
#include "renderer/gpu/accel_structure.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/render_queue.hpp"
#include "renderer/gpu/vma_image.hpp"
#include "renderer/indirect_batch.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/resources/shader_utils.hpp"
#include "renderer/pass/scene_sdf_pass.hpp"
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

    // Probe offsets: total_probes stored as 2D (probes_xy x probes_z)
    int probes_xy = config.probe_counts.x * config.probe_counts.y;
    create_image(probe_offset_image, probe_offset_memory, probe_offset_view,
                 static_cast<uint32_t>(probes_xy),
                 static_cast<uint32_t>(config.probe_counts.z),
                 VK_FORMAT_R16G16B16A16_SFLOAT, storage_sampled);

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

    // Update pipeline layout: descriptor set + push constant (first_frame uint)
    VkPushConstantRange update_push{};
    update_push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    update_push.offset = 0;
    update_push.size = sizeof(uint32_t);

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
        std::array<VkDescriptorSetLayoutBinding, 9> rt_bindings{};
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
                          VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr};
        rt_bindings[6] = {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                          VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, nullptr};
        rt_bindings[7] = {7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                          VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, nullptr};
        rt_bindings[8] = {8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                          VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, nullptr};

        // Enable UPDATE_AFTER_BIND on bindings updated every frame
        std::array<VkDescriptorBindingFlags, 9> rt_bind_flags{};
        rt_bind_flags[1] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; // TLAS
        rt_bind_flags[8] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; // per-instance data

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
    safe_destroy(vkDestroyPipelineLayout, trace_pipeline_layout_);
    safe_destroy(vkDestroyPipelineLayout, update_pipeline_layout_);
    safe_destroy(vkDestroyPipelineLayout, sample_pipeline_layout_);
    safe_destroy(vkDestroyPipelineLayout, rt_trace_pipeline_layout_);
    safe_destroy(vkDestroyDescriptorSetLayout, trace_set_layout_);
    safe_destroy(vkDestroyDescriptorSetLayout, update_set_layout_);
    safe_destroy(vkDestroyDescriptorSetLayout, sample_set_layout_);
    safe_destroy(vkDestroyDescriptorSetLayout, rt_trace_set_layout_);
    safe_destroy(vkDestroySampler, linear_sampler_);
    sbt_buffer_.reset();

    vk_device_ = VK_NULL_HANDLE;
    device_ = nullptr;
}

void DdgiPass::record(const FrameContext& /*ctx*/) {
    // DDGI uses record_into() with per-viewport Images
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
    constants.debug_options = 0;
    constants.irr_tex_w = imgs.irr_width;
    constants.irr_tex_h = imgs.irr_height;
    constants.irr_side = config_.irradiance_side_length;
    constants.probe_rays = config_.probe_rays;
    constants.vis_tex_w = imgs.vis_width;
    constants.vis_tex_h = imgs.vis_height;
    constants.vis_side = config_.visibility_side_length;

    // Temporal amortization: cycle through 4 batches of probes
    constexpr int BATCH_COUNT = 4;
    int total = imgs.total_probes;
    int batch_index = static_cast<int>(frame_counter_ % BATCH_COUNT);
    int probes_per_batch = (total + BATCH_COUNT - 1) / BATCH_COUNT;
    int offset = batch_index * probes_per_batch;
    int count = std::min(probes_per_batch, total - offset);
    constants.probe_update_offset = offset;
    constants.probe_update_count = count;

    constants.hysteresis = config_.hysteresis;
    constants.infinite_bounces_mult = 0.0f;

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

void DdgiPass::record_into(const FrameContext& ctx, Images& imgs) {
    if (trace_pipeline_ == VK_NULL_HANDLE) return;
    if (imgs.radiance_image == VK_NULL_HANDLE) return;

    // Allocate descriptor pool + sets for this Images instance if needed
    if (imgs.desc_pool == VK_NULL_HANDLE) {
        bool has_rt = rt_trace_set_layout_ != VK_NULL_HANDLE;
        uint32_t max_sets = has_rt ? 4u : 3u;

        std::array<VkDescriptorPoolSize, 5> pool_sizes{};
        pool_sizes[0] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, has_rt ? 4u : 3u};
        pool_sizes[1] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, has_rt ? 11u : 9u};
        pool_sizes[2] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, has_rt ? 6u : 5u};
        pool_sizes[3] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, has_rt ? 7u : 3u};
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
            std::array layouts = {trace_set_layout_, rt_trace_set_layout_, update_set_layout_, sample_set_layout_};
            std::array<VkDescriptorSet, 4> sets{};

            VkDescriptorSetAllocateInfo alloc_info{};
            alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            alloc_info.descriptorPool = imgs.desc_pool;
            alloc_info.descriptorSetCount = 4;
            alloc_info.pSetLayouts = layouts.data();

            vkAllocateDescriptorSets(vk_device_, &alloc_info, sets.data());
            imgs.trace_set = sets[0];
            imgs.rt_trace_set = sets[1];
            imgs.update_set = sets[2];
            imgs.sample_set = sets[3];
        } else {
            std::array layouts = {trace_set_layout_, update_set_layout_, sample_set_layout_};
            std::array<VkDescriptorSet, 3> sets{};

            VkDescriptorSetAllocateInfo alloc_info{};
            alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            alloc_info.descriptorPool = imgs.desc_pool;
            alloc_info.descriptorSetCount = 3;
            alloc_info.pSetLayouts = layouts.data();

            vkAllocateDescriptorSets(vk_device_, &alloc_info, sets.data());
            imgs.trace_set = sets[0];
            imgs.update_set = sets[1];
            imgs.sample_set = sets[2];
        }
        imgs.images_need_init = true;
    }

    // Update constants UBO
    update_constants(imgs, ctx);

    // Write descriptors on first use or when views become available
    if (imgs.images_need_init) {
        if (ctx.sdf_view == VK_NULL_HANDLE) {
            FJELL_GFX_WARN("DDGI: SDF view not available, skipping");
            frame_counter_++;
            return;
        }
        if (ctx.sky_cubemap_view == VK_NULL_HANDLE) {
            FJELL_GFX_WARN("DDGI: Sky cubemap view not available, skipping");
            frame_counter_++;
            return;
        }
        FJELL_GFX_DEBUG("DDGI: Writing descriptors (first init)");
    }
    if (imgs.images_need_init && ctx.sdf_view != VK_NULL_HANDLE && ctx.sky_cubemap_view != VK_NULL_HANDLE) {
        write_trace_descriptors(vk_device_, imgs.trace_set, imgs,
                                ctx.sdf_view, ctx.sdf_sampler,
                                ctx.sky_cubemap_view, ctx.sky_cubemap_sampler,
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

            std::array<VkWriteDescriptorSet, 6> sw{};
            for (auto& w : sw) { w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = imgs.sample_set; w.descriptorCount = 1; }
            sw[0].dstBinding = 0; sw[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; sw[0].pBufferInfo = &s_ubo;
            sw[1].dstBinding = 3; sw[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sw[1].pImageInfo = &s_irr;
            sw[2].dstBinding = 4; sw[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sw[2].pImageInfo = &s_vis;
            sw[3].dstBinding = 5; sw[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sw[3].pImageInfo = &s_offset;
            sw[4].dstBinding = 6; sw[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; sw[4].pBufferInfo = &s_status;
            sw[5].dstBinding = 7; sw[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; sw[5].pImageInfo = &s_indirect;

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
            rt_sky.sampler = ctx.sky_cubemap_sampler;
            rt_sky.imageView = ctx.sky_cubemap_view;
            rt_sky.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkDescriptorBufferInfo rt_status{};
            rt_status.buffer = imgs.probe_status_buffer->handle();
            rt_status.range = VK_WHOLE_SIZE;

            VkDescriptorImageInfo rt_offset{};
            rt_offset.sampler = linear_sampler_;
            rt_offset.imageView = imgs.probe_offset_view;
            rt_offset.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            // Vertex + index buffers for closest-hit normal computation
            VkDescriptorBufferInfo rt_verts{};
            VkDescriptorBufferInfo rt_indices{};
            if (ctx.mega_buffer) {
                rt_verts.buffer = ctx.mega_buffer->vertex_buffer_handle();
                rt_verts.range = VK_WHOLE_SIZE;
                rt_indices.buffer = ctx.mega_buffer->index_buffer_handle();
                rt_indices.range = VK_WHOLE_SIZE;
            }

            // Write bindings 0, 2-7 (skip binding 1 = TLAS, updated per-frame)
            std::array<VkWriteDescriptorSet, 7> rtw{};
            for (auto& w : rtw) { w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = imgs.rt_trace_set; w.descriptorCount = 1; }
            rtw[0].dstBinding = 0; rtw[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; rtw[0].pBufferInfo = &rt_ubo;
            rtw[1].dstBinding = 2; rtw[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; rtw[1].pImageInfo = &rt_radiance;
            rtw[2].dstBinding = 3; rtw[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; rtw[2].pImageInfo = &rt_sky;
            rtw[3].dstBinding = 4; rtw[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; rtw[3].pBufferInfo = &rt_status;
            rtw[4].dstBinding = 5; rtw[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; rtw[4].pImageInfo = &rt_offset;
            rtw[5].dstBinding = 6; rtw[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; rtw[5].pBufferInfo = &rt_verts;
            rtw[6].dstBinding = 7; rtw[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; rtw[6].pBufferInfo = &rt_indices;

            vkUpdateDescriptorSets(vk_device_, static_cast<uint32_t>(rtw.size()), rtw.data(), 0, nullptr);
        }

        is_first_frame_ = true;
        imgs.images_need_init = false;
    }

    // SDF backend requires the SDF view
    if (backend_ == DdgiBackend::sdf_fallback && ctx.sdf_view == VK_NULL_HANDLE) {
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
    if (is_first_frame_) {
        constexpr uint32_t PROBE_STATUS_ACTIVE = 4;
        VkDeviceSize status_size = static_cast<VkDeviceSize>(imgs.total_probes) * sizeof(uint32_t);
        vkCmdFillBuffer(ctx.cmd, imgs.probe_status_buffer->handle(), 0, status_size, PROBE_STATUS_ACTIVE);

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
                  is_first_frame_ ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_2_SHADER_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT);

    // ── 2. Build TLAS + dispatch trace (RT or SDF) ──
    // Build TLAS if RT backend is active
    if (backend_ == DdgiBackend::ray_traced && accel_manager_ && ctx.queue && ctx.mega_buffer) {
        const auto& cmds = ctx.queue->commands();
        if (!cmds.empty()) {
            // Ensure BLAS for all visible models
            for (const auto& dc : cmds) {
                if (dc.model) {
                    accel_manager_->ensure_blas(dc.model, *ctx.mega_buffer);
                }
            }
            // Build TLAS — collect draw data + model pointers
            std::vector<GpuDrawData> tlas_draws;
            std::vector<const Model*> tlas_models;
            tlas_draws.reserve(cmds.size());
            tlas_models.reserve(cmds.size());
            for (const auto& dc : cmds) {
                if (!dc.model) continue;
                // Use LOD 0 for TLAS (matches BLAS)
                GpuDrawData data{};
                data.model = dc.push.model;
                data.material_id = dc.push.material_id;
                tlas_draws.push_back(data);
                tlas_models.push_back(dc.model);
            }
            if (!tlas_draws.empty()) {
                accel_manager_->build_tlas(ctx.cmd, tlas_draws.data(),
                                           static_cast<uint32_t>(tlas_draws.size()),
                                           tlas_models);
            }
        }
    }

    bool use_rt = backend_ == DdgiBackend::ray_traced
                  && rt_pipeline_ != VK_NULL_HANDLE
                  && accel_manager_ && accel_manager_->tlas_valid()
                  && imgs.rt_trace_set != VK_NULL_HANDLE;

    if (use_rt) {
        // Update TLAS descriptor (changes every frame)
        VkWriteDescriptorSetAccelerationStructureKHR tlas_write_as{};
        tlas_write_as.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
        tlas_write_as.accelerationStructureCount = 1;
        VkAccelerationStructureKHR tlas = accel_manager_->tlas();
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
        inst_data_info.buffer = accel_manager_->instance_data_buffer();
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
        rt_push.light_direction = glm::vec3(0.5f, 0.8f, 0.3f);
        rt_push.light_intensity = 1.0f;
        rt_push.light_color = glm::vec3(1.0f);
        rt_push.max_trace_dist = 40.0f;

        vkCmdPushConstants(ctx.cmd, rt_trace_pipeline_layout_,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR,
                           0, sizeof(rt_push), &rt_push);

        uint32_t width = static_cast<uint32_t>(config_.probe_rays);
        uint32_t height = static_cast<uint32_t>(constants->probe_update_count);
        device_->cmd_trace_rays_fn()(ctx.cmd,
                                     &sbt_raygen_, &sbt_miss_, &sbt_hit_, &sbt_callable_,
                                     width, height, 1);
    } else {
        // SDF fallback trace
        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, trace_pipeline_);
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                trace_pipeline_layout_, 0, 1, &imgs.trace_set, 0, nullptr);

        TracePushConstants push{};
        push.light_direction = glm::vec3(0.5f, 0.8f, 0.3f);
        push.light_intensity = 1.0f;
        push.light_color = glm::vec3(1.0f);
        push.sdf_resolution = static_cast<float>(SceneSdfPass::DEFAULT_RESOLUTION);
        push.sdf_volume_min = glm::vec3(-10.0f);
        push.sdf_volume_max = glm::vec3(10.0f);

        vkCmdPushConstants(ctx.cmd, trace_pipeline_layout_,
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);

        uint32_t groups_x = (static_cast<uint32_t>(config_.probe_rays) + 31) / 32;
        uint32_t groups_y = static_cast<uint32_t>(constants->probe_update_count);
        vkCmdDispatch(ctx.cmd, groups_x, groups_y, 1);
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

    uint32_t first_frame_val = is_first_frame_ ? 1u : 0u;

    // ── 5. Dispatch irradiance update ──
    vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, irradiance_update_pipeline_);
    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            update_pipeline_layout_, 0, 1, &imgs.update_set, 0, nullptr);
    vkCmdPushConstants(ctx.cmd, update_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(uint32_t), &first_frame_val);
    vkCmdDispatch(ctx.cmd,
                  (static_cast<uint32_t>(imgs.irr_width) + 7) / 8,
                  (static_cast<uint32_t>(imgs.irr_height) + 7) / 8, 1);

    // ── 6. Dispatch visibility update ──
    vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, visibility_update_pipeline_);
    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            update_pipeline_layout_, 0, 1, &imgs.update_set, 0, nullptr);
    vkCmdPushConstants(ctx.cmd, update_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(uint32_t), &first_frame_val);
    vkCmdDispatch(ctx.cmd,
                  (static_cast<uint32_t>(imgs.vis_width) + 7) / 8,
                  (static_cast<uint32_t>(imgs.vis_height) + 7) / 8, 1);

    // ── 7. Barrier for probe offset/status computation ──
    barrier_memory();
    barrier_image(imgs.probe_offset_image,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                  VK_ACCESS_2_SHADER_READ_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);

    // ── 8. Dispatch probe offsets ──
    uint32_t total = static_cast<uint32_t>(imgs.total_probes);
    vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, probe_offset_pipeline_);
    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            update_pipeline_layout_, 0, 1, &imgs.update_set, 0, nullptr);
    vkCmdPushConstants(ctx.cmd, update_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(uint32_t), &first_frame_val);
    vkCmdDispatch(ctx.cmd, (total + 31) / 32, 1, 1);

    // ── 9. Dispatch probe status ──
    vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, probe_status_pipeline_);
    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            update_pipeline_layout_, 0, 1, &imgs.update_set, 0, nullptr);
    vkCmdPushConstants(ctx.cmd, update_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(uint32_t), &first_frame_val);
    vkCmdDispatch(ctx.cmd, (total + 31) / 32, 1, 1);

    // ── 10. Transition indirect image to GENERAL for sample output ──
    barrier_image(imgs.indirect_image,
                  is_first_frame_ ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_IMAGE_LAYOUT_GENERAL,
                  VK_ACCESS_2_SHADER_READ_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);

    // ── 11. Update per-frame sample set descriptors (depth + normal change with viewport) ──
    {
        VkDescriptorImageInfo s_depth{};
        s_depth.sampler = linear_sampler_;
        s_depth.imageView = ctx.target->depth_view();
        s_depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo s_normal{};
        s_normal.sampler = linear_sampler_;
        s_normal.imageView = ctx.target->normal_view();
        s_normal.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        std::array<VkWriteDescriptorSet, 2> sw{};
        for (auto& w : sw) { w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = imgs.sample_set; w.descriptorCount = 1; }
        sw[0].dstBinding = 1; sw[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sw[0].pImageInfo = &s_depth;
        sw[1].dstBinding = 2; sw[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sw[1].pImageInfo = &s_normal;

        vkUpdateDescriptorSets(vk_device_, static_cast<uint32_t>(sw.size()), sw.data(), 0, nullptr);
    }

    // ── 12. Dispatch fullscreen sampling ──
    uint32_t half_res_val = config_.half_res_output ? 1u : 0u;
    vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sample_pipeline_);
    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            sample_pipeline_layout_, 0, 1, &imgs.sample_set, 0, nullptr);
    vkCmdPushConstants(ctx.cmd, sample_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(uint32_t), &half_res_val);
    vkCmdDispatch(ctx.cmd,
                  (imgs.indirect_extent.width + 7) / 8,
                  (imgs.indirect_extent.height + 7) / 8, 1);

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

} // namespace fjell
