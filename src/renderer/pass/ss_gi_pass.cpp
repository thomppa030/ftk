#include "renderer/pass/ss_gi_pass.hpp"
#include "gpu/vulkan/native.hpp"
#include "core/profiler.hpp"
#include "renderer/pass/pass_gpu_zone.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/gpu/vma_image.hpp"
#include "renderer/accel_structure.hpp"
#include "renderer/gpu_context.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/pass_registry.hpp"
#include "renderer/gpu/shader_utils.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/post_process_resources.hpp"
#include "renderer/frame_graph.hpp"
#include "renderer/resources/mega_buffer.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"
#include "scene/scene.hpp"

#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>

#include <cstring>

namespace fjell {

// ── Images lifecycle ────────────────────────────────────────────────────

void SsGiPass::Images::create(gpu::Device& gpu, VkDevice device, VmaAllocator allocator,
                                const SsGiConfig& config,
                                uint32_t screen_w, uint32_t screen_h) {
    probe_count = {
        (screen_w + config.probe_spacing - 1) / config.probe_spacing,
        (screen_h + config.probe_spacing - 1) / config.probe_spacing
    };
    rays_per_probe = config.rays_per_probe;

    uint32_t half_w = std::max(1u, screen_w / 2);
    uint32_t half_h = std::max(1u, screen_h / 2);
    indirect_extent = {half_w, half_h};

    // Returns what the image was made as.
    auto make_image = [&](uint32_t w, uint32_t h, VkFormat format,
                          VkImageUsageFlags usage,
                          VkImage& img, VmaAllocation& mem, VkImageView& view) {
        VkImageCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = format;
        ci.extent = {w, h, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = usage;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        create_vma_image(allocator, &ci, &img, &mem);

        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(device, &vi, nullptr, &view);
        return ci;
    };

    VkImageUsageFlags storage_sampled = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImageUsageFlags storage_sampled_transfer = storage_sampled |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    // Raw radiance: probe_count.x wide, probe_count.y * rays_per_probe tall
    make_image(probe_count.x, probe_count.y * rays_per_probe,
               VK_FORMAT_R16G16B16A16_SFLOAT, storage_sampled,
               raw_radiance_image, raw_radiance_memory, raw_radiance_view);

    // Resolved: probe_count.x * probe_count.y (needs transfer src for history copy)
    make_image(probe_count.x, probe_count.y,
               VK_FORMAT_R16G16B16A16_SFLOAT, storage_sampled_transfer,
               resolved_image, resolved_memory, resolved_view);

    // History: same as resolved (needs transfer dst for history copy)
    make_image(probe_count.x, probe_count.y,
               VK_FORMAT_R16G16B16A16_SFLOAT, storage_sampled_transfer,
               history_image, history_memory, history_view);

    // Indirect output: half-res
    const VkImageCreateInfo indirect_info =
        make_image(half_w, half_h, VK_FORMAT_R16G16B16A16_SFLOAT, storage_sampled,
                   indirect_image, indirect_memory, indirect_view);
    indirect_texture = gpu::vulkan::adopt(gpu, indirect_image, indirect_view, indirect_info);

    // Constants UBO, one per frame slot
    for (auto& ubo : constants_ubo) {
        ubo = std::make_unique<Buffer>(
            allocator, sizeof(GpuSsGiConstants),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
        ubo->map();
    }

    first_frame = true;
    // A new history image holds nothing to accumulate onto.
    history_valid = false;
}

void SsGiPass::Images::resize(gpu::Device& gpu, VkDevice device, VmaAllocator allocator,
                               const SsGiConfig& config,
                               uint32_t screen_w, uint32_t screen_h) {
    if (indirect_image == VK_NULL_HANDLE) {
        create(gpu, device, allocator, config, screen_w, screen_h);
        return;
    }

    // Probe count drives the size of every probe image, and the probe grid is
    // what maps screen pixels to probes — keeping a stale grid stretches it
    // across the new viewport and quantises the result into probe-sized blocks.
    glm::uvec2 wanted{
        (screen_w + config.probe_spacing - 1) / config.probe_spacing,
        (screen_h + config.probe_spacing - 1) / config.probe_spacing
    };
    uint32_t wanted_half_w = std::max(1u, screen_w / 2);
    uint32_t wanted_half_h = std::max(1u, screen_h / 2);
    if (wanted == probe_count
        && wanted_half_w == indirect_extent.width
        && wanted_half_h == indirect_extent.height) {
        return;
    }

    // Descriptor sets reference the old views, so they have to be rebuilt too;
    // destroy() drops the pool and first_frame re-writes them.
    vkDeviceWaitIdle(device);
    destroy(device, allocator);
    create(gpu, device, allocator, config, screen_w, screen_h);
}

void SsGiPass::Images::destroy(VkDevice device, VmaAllocator allocator) {
    auto safe_view = [&](VkImageView& v) { if (v) { vkDestroyImageView(device, v, nullptr); v = VK_NULL_HANDLE; } };
    auto safe_img = [&](VkImage& i, VmaAllocation& a) { destroy_vma_image(allocator, i, a); i = VK_NULL_HANDLE; a = VK_NULL_HANDLE; };

    safe_view(raw_radiance_view); safe_img(raw_radiance_image, raw_radiance_memory);
    safe_view(resolved_view); safe_img(resolved_image, resolved_memory);
    safe_view(history_view); safe_img(history_image, history_memory);
    indirect_texture.reset();
    safe_view(indirect_view); safe_img(indirect_image, indirect_memory);
    safe_view(prev_depth_view); safe_img(prev_depth_image, prev_depth_memory);
    for (auto& ubo : constants_ubo) ubo.reset();

    if (desc_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, desc_pool, nullptr);
        desc_pool = VK_NULL_HANDLE;
    }
    trace_set.fill(VK_NULL_HANDLE);
    resolve_set.fill(VK_NULL_HANDLE);
    upsample_set.fill(VK_NULL_HANDLE);

    invalidate_imgui_texture();
}

VkDescriptorSet SsGiPass::Images::indirect_imgui_texture(VkSampler sampler) {
    if (!indirect_imgui_desc && indirect_view) {
        indirect_imgui_desc = ImGui_ImplVulkan_AddTexture(
            sampler, indirect_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return indirect_imgui_desc;
}

void SsGiPass::Images::invalidate_imgui_texture() {
    if (indirect_imgui_desc) {
        ImGui_ImplVulkan_RemoveTexture(indirect_imgui_desc);
        indirect_imgui_desc = VK_NULL_HANDLE;
    }
}

// ── Pass create ─────────────────────────────────────────────────────────

Result<> SsGiPass::create(Device& device, const PassResources& resources) {
    device_ = &device;
    vk_device_ = device.handle();
    allocator_ = resources.allocator;

    // ── Samplers ──
    VkSamplerCreateInfo sampler_ci{};
    sampler_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_ci.magFilter = VK_FILTER_LINEAR;
    sampler_ci.minFilter = VK_FILTER_LINEAR;
    sampler_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(vk_device_, &sampler_ci, nullptr, &linear_sampler_);

    sampler_ci.magFilter = VK_FILTER_NEAREST;
    sampler_ci.minFilter = VK_FILTER_NEAREST;
    vkCreateSampler(vk_device_, &sampler_ci, nullptr, &nearest_sampler_);

    // ── Trace compute pipeline (ray queries) ──
    if (device.ray_tracing_supported()) {
        // Descriptor set layout for trace:
        // 0: UBO, 1: TLAS, 2: depth, 3: normal, 5: radiance_out
        // 6: vertex SSBO, 7: index SSBO, 8: instance data SSBO
        // Binding 4 is unused: it held a previous-frame color image the trace
        // never sampled. The remaining binding numbers match the shader.
        const VkShaderStageFlags trace_stages = VK_SHADER_STAGE_COMPUTE_BIT;
        std::array<VkDescriptorSetLayoutBinding, 8> trace_bindings{};
        trace_bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, trace_stages, nullptr};
        trace_bindings[1] = {1, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, trace_stages, nullptr};
        trace_bindings[2] = {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, trace_stages, nullptr};
        trace_bindings[3] = {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, trace_stages, nullptr};
        trace_bindings[4] = {5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, trace_stages, nullptr};
        trace_bindings[5] = {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, trace_stages, nullptr};
        trace_bindings[6] = {7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, trace_stages, nullptr};
        trace_bindings[7] = {8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, trace_stages, nullptr};

        std::array<VkDescriptorBindingFlags, 8> trace_flags{};
        trace_flags[1] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; // TLAS
        trace_flags[2] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; // depth (per-frame)
        trace_flags[3] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; // normal (per-frame)
        trace_flags[7] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; // instance data

        VkDescriptorSetLayoutBindingFlagsCreateInfo flags_ci{};
        flags_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        flags_ci.bindingCount = static_cast<uint32_t>(trace_flags.size());
        flags_ci.pBindingFlags = trace_flags.data();

        VkDescriptorSetLayoutCreateInfo layout_ci{};
        layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_ci.pNext = &flags_ci;
        layout_ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        layout_ci.bindingCount = static_cast<uint32_t>(trace_bindings.size());
        layout_ci.pBindings = trace_bindings.data();
        vkCreateDescriptorSetLayout(vk_device_, &layout_ci, nullptr, &trace_set_layout_);

        // Pipeline layout: set 0 = trace set, set 1 = bindless (materials)
        VkPushConstantRange push{};
        push.stageFlags = trace_stages;
        push.size = sizeof(TracePushConstants);

        std::array trace_layouts = {trace_set_layout_, resources.material_set_layout};
        VkPipelineLayoutCreateInfo pipe_ci{};
        pipe_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipe_ci.setLayoutCount = static_cast<uint32_t>(trace_layouts.size());
        pipe_ci.pSetLayouts = trace_layouts.data();
        pipe_ci.pushConstantRangeCount = 1;
        pipe_ci.pPushConstantRanges = &push;
        vkCreatePipelineLayout(vk_device_, &pipe_ci, nullptr, &trace_pipeline_layout_);

        auto spirv = read_spirv(base_path::resolve("shaders/ss_gi_trace.comp.spv"));
        auto mod = create_shader_module(vk_device_, spirv);
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = mod;
        stage.pName = "main";
        VkComputePipelineCreateInfo cci{};
        cci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cci.stage = stage;
        cci.layout = trace_pipeline_layout_;
        VkResult result = vkCreateComputePipelines(vk_device_, resources.pipeline_cache, 1, &cci,
                                                   nullptr, &trace_pipeline_);
        vkDestroyShaderModule(vk_device_, mod, nullptr);
        if (result != VK_SUCCESS) {
            FJELL_GFX_WARN("Failed to create the SS-GI trace pipeline");
            trace_pipeline_ = VK_NULL_HANDLE;
        } else {
            FJELL_GFX_INFO("SS-GI trace pipeline created (ray queries)");
        }
    }

    // ── Resolve compute pipeline ──
    {
        std::array<VkDescriptorSetLayoutBinding, 7> bindings{};
        for (uint32_t i = 0; i < 5; ++i)
            bindings[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[5] = {5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        // Reprojection matrices come from the same constants UBO the trace
        // uses rather than push constants, which cannot hold three mat4s.
        bindings[6] = {6, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

        std::array<VkDescriptorBindingFlags, 7> resolve_flags{};
        resolve_flags[2] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; // depth
        resolve_flags[3] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; // normal
        resolve_flags[4] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; // prev_depth

        VkDescriptorSetLayoutBindingFlagsCreateInfo resolve_flags_ci{};
        resolve_flags_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        resolve_flags_ci.bindingCount = static_cast<uint32_t>(resolve_flags.size());
        resolve_flags_ci.pBindingFlags = resolve_flags.data();

        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.pNext = &resolve_flags_ci;
        lci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        lci.bindingCount = static_cast<uint32_t>(bindings.size());
        lci.pBindings = bindings.data();
        vkCreateDescriptorSetLayout(vk_device_, &lci, nullptr, &resolve_set_layout_);

        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ResolvePushConstants)};
        VkPipelineLayoutCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pci.setLayoutCount = 1;
        pci.pSetLayouts = &resolve_set_layout_;
        pci.pushConstantRangeCount = 1;
        pci.pPushConstantRanges = &push;
        vkCreatePipelineLayout(vk_device_, &pci, nullptr, &resolve_pipeline_layout_);

        auto spirv = read_spirv(base_path::resolve("shaders/ss_gi_resolve.comp.spv"));
        auto mod = create_shader_module(vk_device_, spirv);
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = mod;
        stage.pName = "main";
        VkComputePipelineCreateInfo cci{};
        cci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cci.stage = stage;
        cci.layout = resolve_pipeline_layout_;
        vkCreateComputePipelines(vk_device_, resources.pipeline_cache, 1, &cci, nullptr, &resolve_pipeline_);
        vkDestroyShaderModule(vk_device_, mod, nullptr);
    }

    // ── Upsample compute pipeline ──
    {
        std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
        for (uint32_t i = 0; i < 3; ++i)
            bindings[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

        std::array<VkDescriptorBindingFlags, 4> upsample_flags{};
        upsample_flags[1] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; // depth
        upsample_flags[2] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; // normal

        VkDescriptorSetLayoutBindingFlagsCreateInfo upsample_flags_ci{};
        upsample_flags_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        upsample_flags_ci.bindingCount = static_cast<uint32_t>(upsample_flags.size());
        upsample_flags_ci.pBindingFlags = upsample_flags.data();

        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.pNext = &upsample_flags_ci;
        lci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        lci.bindingCount = static_cast<uint32_t>(bindings.size());
        lci.pBindings = bindings.data();
        vkCreateDescriptorSetLayout(vk_device_, &lci, nullptr, &upsample_set_layout_);

        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(UpsamplePushConstants)};
        VkPipelineLayoutCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pci.setLayoutCount = 1;
        pci.pSetLayouts = &upsample_set_layout_;
        pci.pushConstantRangeCount = 1;
        pci.pPushConstantRanges = &push;
        vkCreatePipelineLayout(vk_device_, &pci, nullptr, &upsample_pipeline_layout_);

        auto spirv = read_spirv(base_path::resolve("shaders/ss_gi_upsample.comp.spv"));
        auto mod = create_shader_module(vk_device_, spirv);
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = mod;
        stage.pName = "main";
        VkComputePipelineCreateInfo cci{};
        cci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cci.stage = stage;
        cci.layout = upsample_pipeline_layout_;
        vkCreateComputePipelines(vk_device_, resources.pipeline_cache, 1, &cci, nullptr, &upsample_pipeline_);
        vkDestroyShaderModule(vk_device_, mod, nullptr);
    }

    FJELL_GFX_DEBUG("SS-GI pass created");
    return {};
}

void SsGiPass::destroy() {
    if (vk_device_ == VK_NULL_HANDLE) return;

    auto safe_destroy = [&](auto fn, auto& h) { if (h) { fn(vk_device_, h, nullptr); h = VK_NULL_HANDLE; } };

    safe_destroy(vkDestroyPipeline, trace_pipeline_);
    safe_destroy(vkDestroyPipeline, resolve_pipeline_);
    safe_destroy(vkDestroyPipeline, upsample_pipeline_);
    safe_destroy(vkDestroyPipelineLayout, trace_pipeline_layout_);
    safe_destroy(vkDestroyPipelineLayout, resolve_pipeline_layout_);
    safe_destroy(vkDestroyPipelineLayout, upsample_pipeline_layout_);
    safe_destroy(vkDestroyDescriptorSetLayout, trace_set_layout_);
    safe_destroy(vkDestroyDescriptorSetLayout, resolve_set_layout_);
    safe_destroy(vkDestroyDescriptorSetLayout, upsample_set_layout_);
    safe_destroy(vkDestroySampler, linear_sampler_);
    safe_destroy(vkDestroySampler, nearest_sampler_);
}

// ── Frame lifecycle ─────────────────────────────────────────────────────

void SsGiPass::prepare_frame(const FrameContext& /*ctx*/, GpuContext& /*gpu*/) {
    // Nothing scene-specific to prepare
}

void SsGiPass::pre_record(const FrameContext& /*ctx*/, GpuContext& /*gpu*/) {
    // TLAS building happens in DDGI's pre_record — we reuse the same TLAS
}

void SsGiPass::update_constants(Images& imgs, const FrameContext& ctx) {
    GpuSsGiConstants constants{};
    constants.view = ctx.view;
    constants.projection = ctx.projection;

    // Use the inv_view_proj from the FrameContext — it matches the depth buffer's
    // projection (including TAA jitter). ctx.view/projection may be identity if
    // the FrameContext wasn't fully populated.
    constants.inv_view_proj = ctx.inv_view_proj;

    constants.prev_view_proj = imgs.prev_view_proj;
    constants.prev_inv_view_proj = imgs.prev_inv_view_proj;
    constants.camera_position = ctx.camera_position;
    constants.max_trace_dist = config_.max_trace_dist;
    // Resolution must match what the SS-GI images were created with (viewport size),
    // not the offscreen render target size (which may be the full window).
    constants.resolution = glm::vec2(imgs.indirect_extent.width * 2,
                                     imgs.indirect_extent.height * 2);
    constants.probe_spacing = config_.probe_spacing;
    constants.frame_index = frame_counter_;
    constants.probe_count = imgs.probe_count;
    constants.trace_cell_size = trace_cell_size_;

    // Sun light for closest-hit direct lighting (cached via set_sun_light)
    constants.sun_direction_intensity = glm::vec4(
        glm::normalize(-sun_direction_), sun_intensity_);
    constants.sun_color = glm::vec4(sun_color_, 0.0f);

    std::memcpy(imgs.constants_ubo[ctx.frame_index]->mapped(), &constants, sizeof(constants));

    // The resolve reprojects through these, so it needs this frame's inverse
    // view-projection alongside the previous frame's forward matrix.
    imgs.inv_view_proj = ctx.inv_view_proj;

    // Store current frame's VP for next frame's prev_view_proj.
    // ctx.inv_view_proj is the correct jittered matrix from the GlobalUBO;
    // ctx.view/projection may be identity if the FrameContext wasn't fully populated.
    prev_view_proj_ = glm::inverse(ctx.inv_view_proj);
}

void SsGiPass::record_depth_history(const FrameContext& ctx, Images& imgs) {
    // Nothing to copy into before the first resolve has created the image,
    // nor while it keeps the old size after a resize.
    const VkExtent2D extent = ctx.target->extent();
    if (imgs.prev_depth_image == VK_NULL_HANDLE
        || imgs.depth_extent.width != extent.width
        || imgs.depth_extent.height != extent.height) {
        return;
    }
    FJELL_PROFILE_SCOPE_N("record_ss_gi_depth_history");
    FJELL_GPU_ZONE(ctx, "SS-GI Depth History");
    VkCommandBuffer cmd = ctx.cmd;

    // This frame's resolve has read the last copy, so it can be overwritten.
    transition_image(cmd, imgs.prev_depth_image,
                     imgs.prev_depth_valid ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                           : VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_IMAGE_ASPECT_DEPTH_BIT);

    VkImageCopy depth_copy{};
    depth_copy.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    depth_copy.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    depth_copy.extent = {extent.width, extent.height, 1};
    vkCmdCopyImage(cmd,
                   ctx.target->depth_image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   imgs.prev_depth_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &depth_copy);

    // Where the next frame's resolve samples it.
    transition_image(cmd, imgs.prev_depth_image,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                     VK_IMAGE_ASPECT_DEPTH_BIT);
    imgs.prev_depth_valid = true;
}

void SsGiPass::record(const FrameContext& ctx) {
    if (!active()) { return; }
    auto* imgs = ctx.pp_resources ? ctx.pp_resources->ss_gi() : nullptr;
    if (imgs == nullptr) { return; }
    FJELL_PROFILE_SCOPE_N("record_ss_gi");
    FJELL_GPU_ZONE(ctx, "SS-GI");
    // Stopping before the upsample leaves the output holding what it held.
    record_into(ctx, *imgs);
}

PassSetup SsGiPass::setup() const {
    return {
        .name = "ss_gi",
        .phase = RenderPhase::Color,
        .sub_order = 6, // after DDGI (5), which means after depth/normal are ready
    };
}

void SsGiPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    auto depth = builder.import_named(ctx, "depth");
    auto normal = builder.import_named(ctx, "normal");
    // The trace, the resolve and the upsample all read both in compute.
    builder.read(depth, gpu::Access::sampled_compute);
    builder.read(normal, gpu::Access::sampled_compute);
    // The emissive light the surfaces sample, kept for them into the next
    // frame, which is also what keeps the pass alive. The graph moves it
    // into GENERAL, and back to rest afterwards however record() ends.
    if (active()) {
        if (auto out = builder.import_named_optional(ctx, "ss_gi_output"); out.valid()) {
            builder.write(out, gpu::Access::storage_write_compute);
        }
    }
}

// ── Record ──────────────────────────────────────────────────────────────

void SsGiPass::record_into(const FrameContext& ctx, Images& imgs) {
    if (trace_pipeline_ == VK_NULL_HANDLE) return;
    if (imgs.raw_radiance_image == VK_NULL_HANDLE) return;
    // This frame's TLAS for the scene, built by the rt_scene pass; none when
    // no tracer's reach held anything.
    const VkAccelerationStructureKHR tlas =
        accel_manager_ != nullptr && ctx.scene != nullptr
            ? accel_manager_->tlas(ctx.scene->id(), ctx.frame_index)
            : VK_NULL_HANDLE;
    if (tlas == VK_NULL_HANDLE) return;
    if (!ctx.target) return;

    // All required views must be valid before we can dispatch
    auto depth_view = ctx.target->depth_view();
    auto normal_view = ctx.target->normal_view();
    if (depth_view == VK_NULL_HANDLE || normal_view == VK_NULL_HANDLE) return;

    auto cmd = ctx.cmd;

    // ── Previous-frame depth: create lazily, recreate on resize ──
    // Full-screen depth copy used by the resolve to reject history on
    // disocclusion. Created here (not in Images::create) because the depth
    // format and target extent are only known at record time.
    VkExtent2D target_extent = ctx.target->extent();
    if (imgs.prev_depth_image == VK_NULL_HANDLE ||
        imgs.depth_extent.width != target_extent.width ||
        imgs.depth_extent.height != target_extent.height) {
        if (imgs.prev_depth_view != VK_NULL_HANDLE)
            vkDestroyImageView(vk_device_, imgs.prev_depth_view, nullptr);
        destroy_vma_image(allocator_, imgs.prev_depth_image, imgs.prev_depth_memory);

        VkFormat depth_format = device_->find_depth_format();
        VkImageCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = depth_format;
        ci.extent = {target_extent.width, target_extent.height, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        // DEPTH_STENCIL_ATTACHMENT is required for the DEPTH_STENCIL_READ_ONLY
        // layout this is sampled in, even though it is never rendered to.
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                 | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        create_vma_image(allocator_, &ci, &imgs.prev_depth_image, &imgs.prev_depth_memory);

        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = imgs.prev_depth_image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = depth_format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        vkCreateImageView(vk_device_, &vi, nullptr, &imgs.prev_depth_view);

        imgs.depth_extent = target_extent;
        imgs.prev_depth_valid = false;
    }

    // Update constants
    update_constants(imgs, ctx);

    // ── Allocate descriptor sets on first use ──
    if (imgs.desc_pool == VK_NULL_HANDLE) {
        // One trace, one resolve and one upsample set per frame slot.
        // Sizes are per set, multiplied by the slot count.
        constexpr uint32_t SLOTS = MAX_FRAMES_IN_FLIGHT;
        std::array<VkDescriptorPoolSize, 5> pool_sizes{};
        pool_sizes[0] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 * SLOTS};        // trace + resolve
        pool_sizes[1] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 17 * SLOTS};
        pool_sizes[2] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4 * SLOTS};
        pool_sizes[3] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * SLOTS};
        pool_sizes[4] = {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, SLOTS};

        VkDescriptorPoolCreateInfo pool_ci{};
        pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_ci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        pool_ci.maxSets = 3 * SLOTS;
        pool_ci.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
        pool_ci.pPoolSizes = pool_sizes.data();
        vkCreateDescriptorPool(vk_device_, &pool_ci, nullptr, &imgs.desc_pool);

        std::array<VkDescriptorSetLayout, 3 * SLOTS> layouts{};
        std::array<VkDescriptorSet, 3 * SLOTS> sets{};
        for (uint32_t s = 0; s < SLOTS; ++s) {
            layouts[s] = trace_set_layout_;
            layouts[SLOTS + s] = resolve_set_layout_;
            layouts[2 * SLOTS + s] = upsample_set_layout_;
        }
        VkDescriptorSetAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = imgs.desc_pool;
        alloc_info.descriptorSetCount = static_cast<uint32_t>(layouts.size());
        alloc_info.pSetLayouts = layouts.data();
        vkAllocateDescriptorSets(vk_device_, &alloc_info, sets.data());
        for (uint32_t s = 0; s < SLOTS; ++s) {
            imgs.trace_set[s] = sets[s];
            imgs.resolve_set[s] = sets[SLOTS + s];
            imgs.upsample_set[s] = sets[2 * SLOTS + s];
        }
        imgs.first_frame = true;
    }

    // ── Write static descriptors once ──
    if (imgs.first_frame) {
        // Static bindings that don't change per-frame, written into every
        // slot's trace and resolve set; only the UBO differs per slot.
        VkDescriptorImageInfo radiance_out_info{};
        radiance_out_info.imageView = imgs.raw_radiance_view;
        radiance_out_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkDescriptorImageInfo raw_info{nearest_sampler_, imgs.raw_radiance_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo hist_info{linear_sampler_, imgs.history_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo resolve_out_info{};
        resolve_out_info.imageView = imgs.resolved_view;
        resolve_out_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        for (uint32_t s = 0; s < MAX_FRAMES_IN_FLIGHT; ++s) {
            VkDescriptorBufferInfo ubo_info{imgs.constants_ubo[s]->handle(), 0, sizeof(GpuSsGiConstants)};

            std::array<VkWriteDescriptorSet, 2> sw{};
            for (auto& w : sw) { w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = imgs.trace_set[s]; w.descriptorCount = 1; }
            sw[0].dstBinding = 0; sw[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; sw[0].pBufferInfo = &ubo_info;
            sw[1].dstBinding = 5; sw[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; sw[1].pImageInfo = &radiance_out_info;
            vkUpdateDescriptorSets(vk_device_, static_cast<uint32_t>(sw.size()), sw.data(), 0, nullptr);

            if (ctx.mega_buffer) {
                VkDescriptorBufferInfo vert_info{
                    gpu::vulkan::native_buffer(*ctx.device, ctx.mega_buffer->vertex_buffer()), 0, VK_WHOLE_SIZE};
                VkDescriptorBufferInfo idx_info{
                    gpu::vulkan::native_buffer(*ctx.device, ctx.mega_buffer->index_buffer()), 0, VK_WHOLE_SIZE};
                std::array<VkWriteDescriptorSet, 2> bw{};
                for (auto& w : bw) { w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = imgs.trace_set[s]; w.descriptorCount = 1; }
                bw[0].dstBinding = 6; bw[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; bw[0].pBufferInfo = &vert_info;
                bw[1].dstBinding = 7; bw[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; bw[1].pBufferInfo = &idx_info;
                vkUpdateDescriptorSets(vk_device_, static_cast<uint32_t>(bw.size()), bw.data(), 0, nullptr);
            }

            // Resolve set: raw radiance, history, resolved output, this slot's UBO
            std::array<VkWriteDescriptorSet, 4> rsw{};
            for (auto& w : rsw) { w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = imgs.resolve_set[s]; w.descriptorCount = 1; }
            rsw[0].dstBinding = 0; rsw[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; rsw[0].pImageInfo = &raw_info;
            rsw[1].dstBinding = 1; rsw[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; rsw[1].pImageInfo = &hist_info;
            rsw[2].dstBinding = 5; rsw[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; rsw[2].pImageInfo = &resolve_out_info;
            rsw[3].dstBinding = 6; rsw[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; rsw[3].pBufferInfo = &ubo_info;
            vkUpdateDescriptorSets(vk_device_, static_cast<uint32_t>(rsw.size()), rsw.data(), 0, nullptr);
        }

        // Upsample set: static bindings (resolved probes, indirect output)
        VkDescriptorImageInfo resolved_info{linear_sampler_, imgs.resolved_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo indirect_out_info{};
        indirect_out_info.imageView = imgs.indirect_view;
        indirect_out_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        for (VkDescriptorSet upsample_set : imgs.upsample_set) {
            std::array<VkWriteDescriptorSet, 2> usw{};
            for (auto& w : usw) { w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = upsample_set; w.descriptorCount = 1; }
            usw[0].dstBinding = 0; usw[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; usw[0].pImageInfo = &resolved_info;
            usw[1].dstBinding = 3; usw[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; usw[1].pImageInfo = &indirect_out_info;
            vkUpdateDescriptorSets(vk_device_, static_cast<uint32_t>(usw.size()), usw.data(), 0, nullptr);
        }

        imgs.first_frame = false;
    }

    // ── Update per-frame descriptors on this slot's sets ──
    {
        // Trace set: depth, normal
        VkDescriptorImageInfo di{nearest_sampler_, depth_view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo ni{nearest_sampler_, normal_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

        std::array<VkWriteDescriptorSet, 2> tw{};
        for (auto& w : tw) { w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = imgs.trace_set[ctx.frame_index]; w.descriptorCount = 1; }
        tw[0].dstBinding = 2; tw[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; tw[0].pImageInfo = &di;
        tw[1].dstBinding = 3; tw[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; tw[1].pImageInfo = &ni;
        vkUpdateDescriptorSets(vk_device_, static_cast<uint32_t>(tw.size()), tw.data(), 0, nullptr);

        // Resolve set: depth, normal, prev_depth (previous frame's depth copy)
        VkDescriptorImageInfo pdi{nearest_sampler_, imgs.prev_depth_view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        std::array<VkWriteDescriptorSet, 3> rw{};
        for (auto& w : rw) { w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = imgs.resolve_set[ctx.frame_index]; w.descriptorCount = 1; }
        rw[0].dstBinding = 2; rw[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; rw[0].pImageInfo = &di;
        rw[1].dstBinding = 3; rw[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; rw[1].pImageInfo = &ni;
        rw[2].dstBinding = 4; rw[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; rw[2].pImageInfo = &pdi;
        vkUpdateDescriptorSets(vk_device_, static_cast<uint32_t>(rw.size()), rw.data(), 0, nullptr);

        // Upsample set: depth, normal
        std::array<VkWriteDescriptorSet, 2> uw{};
        for (auto& w : uw) { w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = imgs.upsample_set[ctx.frame_index]; w.descriptorCount = 1; }
        uw[0].dstBinding = 1; uw[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; uw[0].pImageInfo = &di;
        uw[1].dstBinding = 2; uw[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; uw[1].pImageInfo = &ni;
        vkUpdateDescriptorSets(vk_device_, static_cast<uint32_t>(uw.size()), uw.data(), 0, nullptr);
    }

    // ── Barrier helper ──
    auto barrier_image = [&](VkImage image, VkImageLayout old_layout, VkImageLayout new_layout,
                             VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                             VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access,
                             VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
        VkImageMemoryBarrier2 b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.srcStageMask = src_stage;
        b.srcAccessMask = src_access;
        b.dstStageMask = dst_stage;
        b.dstAccessMask = dst_access;
        b.oldLayout = old_layout;
        b.newLayout = new_layout;
        b.image = image;
        b.subresourceRange = {aspect, 0, 1, 0, 1};
        VkDependencyInfo d{};
        d.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        d.imageMemoryBarrierCount = 1;
        d.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &d);
    };

    // ── 1. Pre-trace barrier ──
    barrier_image(imgs.raw_radiance_image,
                  VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);

    // ── 2. Update TLAS + instance data descriptors (per-frame) ──
    {
        VkWriteDescriptorSetAccelerationStructureKHR tlas_write_as{};
        tlas_write_as.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
        tlas_write_as.accelerationStructureCount = 1;
        tlas_write_as.pAccelerationStructures = &tlas;

        VkWriteDescriptorSet tlas_write{};
        tlas_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        tlas_write.dstSet = imgs.trace_set[ctx.frame_index];
        tlas_write.dstBinding = 1;
        tlas_write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        tlas_write.descriptorCount = 1;
        tlas_write.pNext = &tlas_write_as;

        VkDescriptorBufferInfo inst_info{
            accel_manager_->instance_data_buffer(ctx.scene->id(), ctx.frame_index), 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet inst_write{};
        inst_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        inst_write.dstSet = imgs.trace_set[ctx.frame_index];
        inst_write.dstBinding = 8;
        inst_write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        inst_write.descriptorCount = 1;
        inst_write.pBufferInfo = &inst_info;

        std::array writes = {tlas_write, inst_write};
        vkUpdateDescriptorSets(vk_device_, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }

    // ── 3. Dispatch the trace ──
    {
        FJELL_PROFILE_SCOPE_N("ss_gi_trace");
        FJELL_GPU_ZONE(ctx, "SS-GI Trace");
        std::array trace_sets = {imgs.trace_set[ctx.frame_index], ctx.bindless_set};
        TracePushConstants push{};
        push.rays_per_probe = config_.rays_per_probe;
        push.ray_offset = frame_counter_ * config_.rays_per_probe;

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, trace_pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                trace_pipeline_layout_, 0,
                                static_cast<uint32_t>(trace_sets.size()), trace_sets.data(),
                                0, nullptr);
        vkCmdPushConstants(cmd, trace_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(push), &push);

        // One invocation per ray: x and y the probe, z the ray.
        vkCmdDispatch(cmd,
                      (imgs.probe_count.x + 7) / 8,
                      (imgs.probe_count.y + 7) / 8,
                      config_.rays_per_probe);
    }

    // ── 4. Barrier: raw radiance write → read ──
    barrier_image(imgs.raw_radiance_image,
                  VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);

    // Resolved output to GENERAL for write
    barrier_image(imgs.resolved_image,
                  VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);

    // The history rests where the resolve samples it once a frame has copied
    // into it; a new image is moved there once, holding nothing yet.
    if (!imgs.history_valid) {
        barrier_image(imgs.history_image,
                      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_2_NONE, 0,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    }

    // prev_depth holds last frame's depth and must be readable by the resolve.
    // On the first frame after (re)create it has no data yet — seed the layout
    // so the sample is valid; the resolve rejects history while frame_index < 2.
    if (!imgs.prev_depth_valid) {
        barrier_image(imgs.prev_depth_image,
                      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                      VK_IMAGE_ASPECT_DEPTH_BIT);
    }

    // ── 5. Dispatch resolve (temporal accumulation) ──
    {
        FJELL_PROFILE_SCOPE_N("ss_gi_resolve");
        FJELL_GPU_ZONE(ctx, "SS-GI Resolve");
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, resolve_pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                resolve_pipeline_layout_, 0, 1, &imgs.resolve_set[ctx.frame_index], 0, nullptr);

        ResolvePushConstants push{};
        push.probe_count = imgs.probe_count;
        push.rays_per_probe = config_.rays_per_probe;
        push.frame_index = frame_counter_;
        push.resolution = glm::vec2(imgs.indirect_extent.width * 2,
                                    imgs.indirect_extent.height * 2);
        push.probe_spacing = config_.probe_spacing;
        push.hysteresis = imgs.history_valid ? config_.hysteresis : 0.0f;
        vkCmdPushConstants(cmd, resolve_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(push), &push);

        vkCmdDispatch(cmd,
                      (imgs.probe_count.x + 7) / 8,
                      (imgs.probe_count.y + 7) / 8, 1);
    }

    // ── 6. Copy resolved → history for next frame ──
    {
        FJELL_PROFILE_SCOPE_N("ss_gi_history_copy");
        FJELL_GPU_ZONE(ctx, "SS-GI History Copy");
        barrier_image(imgs.resolved_image,
                      VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        barrier_image(imgs.history_image,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                      VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

        VkImageCopy copy_region{};
        copy_region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy_region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy_region.extent = {imgs.probe_count.x, imgs.probe_count.y, 1};
        vkCmdCopyImage(cmd,
                       imgs.resolved_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       imgs.history_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &copy_region);
        barrier_image(imgs.history_image,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    }

    // The resolve has consumed prev_view_proj; the history it just wrote
    // belongs to this frame's camera, so that becomes next frame's reference.
    imgs.prev_view_proj = glm::inverse(imgs.inv_view_proj);
    imgs.prev_inv_view_proj = imgs.inv_view_proj;
    imgs.history_valid = true;

    // ── 7. Barriers for upsample ──
    barrier_image(imgs.resolved_image,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    // The graph has the indirect output in GENERAL.

    // ── 8. Dispatch upsample ──
    {
        FJELL_PROFILE_SCOPE_N("ss_gi_upsample");
        FJELL_GPU_ZONE(ctx, "SS-GI Upsample");
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, upsample_pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                upsample_pipeline_layout_, 0, 1, &imgs.upsample_set[ctx.frame_index], 0, nullptr);

        UpsamplePushConstants push{};
        push.output_size = {imgs.indirect_extent.width, imgs.indirect_extent.height};
        push.probe_count = imgs.probe_count;
        push.probe_spacing = config_.probe_spacing;
        vkCmdPushConstants(cmd, upsample_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(push), &push);

        vkCmdDispatch(cmd,
                      (imgs.indirect_extent.width + 7) / 8,
                      (imgs.indirect_extent.height + 7) / 8, 1);
    }

    frame_counter_++;
}

// ── SsGiDepthHistoryPass ────────────────────────────────────────────────────

Result<> SsGiDepthHistoryPass::create(Device& /*device*/, const PassResources& /*resources*/) {
    return {};
}

void SsGiDepthHistoryPass::destroy() {}

void SsGiDepthHistoryPass::post_create(GpuContext& /*gpu*/, PassRegistry& passes) {
    core_ = &passes.get<SsGiPass>("ss_gi");
}

PassSetup SsGiDepthHistoryPass::setup() const {
    return {
        .name = "ss_gi_depth_history",
        .phase = RenderPhase::Color,
        .sub_order = 7,   // right after SS-GI (6) has read the last copy
    };
}

void SsGiDepthHistoryPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    if (core_ == nullptr || !core_->active()) { return; }
    builder.read(builder.import_named(ctx, "depth"), gpu::Access::copy_src);
    // What it writes is SS-GI's private history, for the next frame.
    builder.has_side_effects();
}

void SsGiDepthHistoryPass::record(const FrameContext& ctx) {
    if (core_ == nullptr || !core_->active() || ctx.target == nullptr) { return; }
    auto* imgs = ctx.pp_resources ? ctx.pp_resources->ss_gi() : nullptr;
    if (imgs == nullptr) { return; }
    core_->record_depth_history(ctx, *imgs);
}

} // namespace fjell
