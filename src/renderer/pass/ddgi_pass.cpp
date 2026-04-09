#include "renderer/pass/ddgi_pass.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/gpu/vma_image.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/resources/shader_utils.hpp"
#include "renderer/pass/scene_sdf_pass.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

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
    destroy(device, allocator);
    create(device, allocator, config, screen_w, screen_h);
}

void DdgiPass::Images::invalidate_imgui_texture() {
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

    FJELL_GFX_DEBUG("DDGI pass created (SDF trace + 4 update pipelines)");
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
    safe_destroy(vkDestroyPipelineLayout, trace_pipeline_layout_);
    safe_destroy(vkDestroyPipelineLayout, update_pipeline_layout_);
    safe_destroy(vkDestroyDescriptorSetLayout, trace_set_layout_);
    safe_destroy(vkDestroyDescriptorSetLayout, update_set_layout_);
    safe_destroy(vkDestroySampler, linear_sampler_);

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

    // Temporal amortization: trace 1/4 of probes per frame
    int total = imgs.total_probes;
    int batches = 4;
    int batch_size = (total + batches - 1) / batches;
    int batch_idx = static_cast<int>(frame_counter_ % batches);
    constants.probe_update_offset = batch_idx * batch_size;
    constants.probe_update_count = std::min(batch_size, total - constants.probe_update_offset);

    constants.hysteresis = config_.hysteresis;
    constants.infinite_bounces_mult = 0.0f;

    // Random rotation for ray jitter
    static std::mt19937 rng{42};
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);
    float angle = dist(rng) * 2.0f * glm::pi<float>();
    glm::vec3 axis = glm::normalize(glm::vec3(dist(rng) - 0.5f, dist(rng) - 0.5f, dist(rng) - 0.5f));
    constants.random_rotation = glm::rotate(glm::mat4(1.0f), angle, axis);

    constants.camera_position = ctx.camera_position;
    constants.inv_view_proj = glm::mat4(1.0f); // filled in sampling pass
    constants.resolution = glm::vec2(ctx.target ? ctx.target->extent().width : 1920,
                                     ctx.target ? ctx.target->extent().height : 1080);

    std::memcpy(imgs.constants_ubo->mapped(), &constants, sizeof(constants));
}

void DdgiPass::record_into(const FrameContext& ctx, Images& imgs) {
    if (trace_pipeline_ == VK_NULL_HANDLE) return;
    if (imgs.radiance_image == VK_NULL_HANDLE) return;

    // Allocate descriptor pool + sets for this Images instance if needed
    if (imgs.desc_pool == VK_NULL_HANDLE) {
        std::array<VkDescriptorPoolSize, 4> pool_sizes{};
        pool_sizes[0] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2};          // trace UBO + update UBO
        pool_sizes[1] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4};  // trace: SDF+sky+offset, update: radiance
        pool_sizes[2] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4};           // trace: radiance, update: irr+vis+offset
        pool_sizes[3] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};          // trace: status, update: status

        VkDescriptorPoolCreateInfo pool_ci{};
        pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_ci.maxSets = 2;
        pool_ci.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
        pool_ci.pPoolSizes = pool_sizes.data();

        vkCreateDescriptorPool(vk_device_, &pool_ci, nullptr, &imgs.desc_pool);

        std::array layouts = {trace_set_layout_, update_set_layout_};
        std::array<VkDescriptorSet, 2> sets{};

        VkDescriptorSetAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = imgs.desc_pool;
        alloc_info.descriptorSetCount = 2;
        alloc_info.pSetLayouts = layouts.data();

        vkAllocateDescriptorSets(vk_device_, &alloc_info, sets.data());
        imgs.trace_set = sets[0];
        imgs.update_set = sets[1];
        imgs.images_need_init = true;
    }

    // Update constants UBO
    update_constants(imgs, ctx);

    // Write descriptors on first use or when views become available
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

        is_first_frame_ = true;
        imgs.images_need_init = false;
    }

    if (ctx.sdf_view == VK_NULL_HANDLE) {
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

    // ── 1. Pre-trace barriers ──
    barrier_image(imgs.radiance_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                  VK_ACCESS_2_SHADER_READ_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
    barrier_image(imgs.probe_offset_image,
                  is_first_frame_ ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_2_SHADER_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT);

    // ── 2. Dispatch SDF trace ──
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

    // ── 3. Barrier: radiance write → read ──
    barrier_image(imgs.radiance_image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_2_SHADER_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT);

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

    // ── 10. Final barrier: all outputs ready for sampling ──
    barrier_memory();

    is_first_frame_ = false;
    frame_counter_++;
}

} // namespace fjell
