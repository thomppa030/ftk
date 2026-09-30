#include "renderer/pass/rt_reflection_pass.hpp"
#include "gpu/vulkan/native.hpp"

#include "core/base_path.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "renderer/accel_structure.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/gpu/shader_utils.hpp"
#include "renderer/gpu/vk_check.hpp"
#include "renderer/gpu/vma_image.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass/ddgi_pass.hpp"
#include "renderer/pass/pass_gpu_zone.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/post_process_resources.hpp"
#include "renderer/resource_registry.hpp"
#include "renderer/resources/mega_buffer.hpp"
#include "scene/scene.hpp"

#include <array>
#include <cstring>

namespace fjell {

namespace {

// Push constant for the trace. Kept small (16 bytes) so we have budget
// elsewhere if needed.
struct ReflectionPush {
    uint32_t frame_index;
    float max_trace_dist;
    float intensity;
    // Size of the coarsest cell in the traced geometry, in metres — the
    // terrain ray mesh's, since it is the one geometry that is traced at a
    // lower resolution than it is shaded. The trace offsets a ray's origin
    // by how far the two representations can disagree over that distance.
    // Zero where no terrain is loaded.
    float trace_cell_size;
};

// UBO at binding 14: sun lighting only. Everything else needed at a hit
// (camera, projection inverse, DDGI grid info) lives in the
// DDGI UBO at binding 0, which we share with DdgiPass.
struct ReflectionUbo {
    glm::vec4 sun_dir_intensity;
    glm::vec4 sun_color;
};

// Push constant for the denoise compute. The reprojection matrices are too
// large for the guaranteed push range and travel in a UBO instead.
struct DenoisePush {
    uint32_t width;
    uint32_t height;
    uint32_t frame_index;
    float hysteresis;
    uint32_t debug_mode;
};

// Push constant for the spatial filter.
struct SpatialPush {
    uint32_t width;
    uint32_t height;
    float strength;
    float depth_sigma;
};

void emit_image_barrier(VkCommandBuffer cmd, VkImage image,
                         VkImageLayout old_layout, VkImageLayout new_layout,
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
}

} // namespace

// ── create ─────────────────────────────────────────────────────────────

Result<> RtReflectionPass::create(Device& device, const PassResources& resources) {
    device_ = &device;
    vk_device_ = device.handle();
    allocator_ = resources.allocator;

    // Samplers (linear for color/normal/MR/atlases, nearest for depth).
    {
        VkSamplerCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        ci.magFilter = VK_FILTER_LINEAR;
        ci.minFilter = VK_FILTER_LINEAR;
        ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(vk_device_, &ci, nullptr, &linear_sampler_) != VK_SUCCESS) {
            return make_error("Failed to create RT reflection linear sampler");
        }
        ci.magFilter = VK_FILTER_NEAREST;
        ci.minFilter = VK_FILTER_NEAREST;
        if (vkCreateSampler(vk_device_, &ci, nullptr, &nearest_sampler_) != VK_SUCCESS) {
            return make_error("Failed to create RT reflection nearest sampler");
        }
    }

    // The trace is only built when the device supports ray tracing. Without
    // it the pass still exists (so .fjgraph parses) but record() returns
    // immediately.
    if (!device.ray_tracing_supported()) {
        FJELL_GFX_INFO("RT reflections disabled (no ray queries)");
        return {};
    }

    // ── Trace descriptor set layout (15 bindings) ──
    constexpr uint32_t BINDING_COUNT = 15;
    constexpr VkShaderStageFlags TRACE = VK_SHADER_STAGE_COMPUTE_BIT;

    std::array<VkDescriptorSetLayoutBinding, BINDING_COUNT> bindings{};
    bindings[0]  = {0,  VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,            1, TRACE, nullptr};
    bindings[1]  = {1,  VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,1, TRACE, nullptr};
    bindings[2]  = {2,  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,    1, TRACE, nullptr};
    bindings[3]  = {3,  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,    1, TRACE, nullptr};
    bindings[4]  = {4,  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,    1, TRACE, nullptr};
    bindings[5]  = {5,  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,    1, TRACE, nullptr};
    bindings[6]  = {6,  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,             1, TRACE, nullptr};
    bindings[7]  = {7,  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,            1, TRACE, nullptr};
    bindings[8]  = {8,  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,            1, TRACE, nullptr};
    bindings[9]  = {9,  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,            1, TRACE, nullptr};
    bindings[10] = {10, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,    1, TRACE, nullptr};
    bindings[11] = {11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,    1, TRACE, nullptr};
    bindings[12] = {12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,    1, TRACE, nullptr};
    bindings[13] = {13, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,            1, TRACE, nullptr};
    bindings[14] = {14, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,            1, TRACE, nullptr};

    // UPDATE_AFTER_BIND on every binding rewritten per-frame. Bindings
    // 7, 8, 14 (mega vertex/index, reflection UBO) are written once at
    // first use and never touched again — those don't need the flag.
    std::array<VkDescriptorBindingFlags, BINDING_COUNT> flags{};
    flags[0]  = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; // DDGI UBO (per-viewport)
    flags[1]  = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; // TLAS
    flags[2]  = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; // depth
    flags[3]  = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; // normal
    flags[4]  = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; // mr
    flags[5]  = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; // sky
    flags[6]  = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; // reflection_out (re-bind on resize)
    flags[9]  = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; // instance data
    flags[10] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; // irradiance
    flags[11] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; // visibility
    flags[12] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; // probe offset
    flags[13] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; // probe status

    VkDescriptorSetLayoutBindingFlagsCreateInfo flags_ci{};
    flags_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flags_ci.bindingCount = static_cast<uint32_t>(flags.size());
    flags_ci.pBindingFlags = flags.data();

    VkDescriptorSetLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_ci.pNext = &flags_ci;
    layout_ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layout_ci.bindingCount = static_cast<uint32_t>(bindings.size());
    layout_ci.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(vk_device_, &layout_ci, nullptr, &trace_set_layout_) != VK_SUCCESS) {
        return make_error("Failed to create RT reflection descriptor layout");
    }

    // ── Pipeline layout: set 0 = trace set, set 1 = bindless materials ──
    VkPushConstantRange push{};
    push.stageFlags = TRACE;
    push.size = sizeof(ReflectionPush);

    std::array<VkDescriptorSetLayout, 2> layouts = {trace_set_layout_, resources.material_set_layout};
    VkPipelineLayoutCreateInfo pl_ci{};
    pl_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl_ci.setLayoutCount = static_cast<uint32_t>(layouts.size());
    pl_ci.pSetLayouts = layouts.data();
    pl_ci.pushConstantRangeCount = 1;
    pl_ci.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(vk_device_, &pl_ci, nullptr, &trace_pipeline_layout_) != VK_SUCCESS) {
        return make_error("Failed to create RT reflection pipeline layout");
    }

    // ── Trace compute pipeline (ray queries) ──
    {
        auto spirv = read_spirv(base_path::resolve("shaders/rt_reflection_trace.comp.spv"));
        auto* mod = create_shader_module(vk_device_, spirv);
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
            return make_error("Failed to create RT reflection trace pipeline");
        }
    }

    // ── Temporal denoise compute pipeline ──
    // One stochastic sample per pixel is unusable raw; this accumulates it
    // across frames. See shaders/rt_reflection_denoise.comp.
    {
        depth_format_ = resources.depth_format;

        std::array<VkDescriptorSetLayoutBinding, 8> dn_bindings{};
        for (uint32_t i = 0; i < 5; ++i) {
            dn_bindings[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                           VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        }
        dn_bindings[5] = {5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        dn_bindings[6] = {6, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        dn_bindings[7] = {7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

        // depth and mr are the render target's own views, which change when a
        // viewport resizes, so they are rebound per frame.
        std::array<VkDescriptorBindingFlags, 8> dn_flags{};
        dn_flags[2] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; // depth
        dn_flags[4] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; // mr

        VkDescriptorSetLayoutBindingFlagsCreateInfo dn_flags_ci{};
        dn_flags_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        dn_flags_ci.bindingCount = static_cast<uint32_t>(dn_flags.size());
        dn_flags_ci.pBindingFlags = dn_flags.data();

        VkDescriptorSetLayoutCreateInfo dn_lci{};
        dn_lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dn_lci.pNext = &dn_flags_ci;
        dn_lci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        dn_lci.bindingCount = static_cast<uint32_t>(dn_bindings.size());
        dn_lci.pBindings = dn_bindings.data();
        vk_check(vkCreateDescriptorSetLayout(vk_device_, &dn_lci, nullptr, &denoise_set_layout_),
                 "RT reflection denoise descriptor set layout");

        VkPushConstantRange dn_push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(DenoisePush)};
        VkPipelineLayoutCreateInfo dn_pci{};
        dn_pci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        dn_pci.setLayoutCount = 1;
        dn_pci.pSetLayouts = &denoise_set_layout_;
        dn_pci.pushConstantRangeCount = 1;
        dn_pci.pPushConstantRanges = &dn_push;
        vk_check(vkCreatePipelineLayout(vk_device_, &dn_pci, nullptr, &denoise_pipeline_layout_),
                 "RT reflection denoise pipeline layout");

        auto spirv = read_spirv(base_path::resolve("shaders/rt_reflection_denoise.comp.spv"));
        auto mod = create_shader_module(vk_device_, spirv);
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = mod;
        stage.pName = "main";
        VkComputePipelineCreateInfo cci{};
        cci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cci.stage = stage;
        cci.layout = denoise_pipeline_layout_;
        vk_check(vkCreateComputePipelines(vk_device_, resources.pipeline_cache, 1, &cci,
                                          nullptr, &denoise_pipeline_),
                 "RT reflection denoise pipeline");
        vkDestroyShaderModule(vk_device_, mod, nullptr);
    }

    // ── Spatial filter compute pipeline ──
    // Reconciles neighbouring pixels the temporal pass converged
    // independently. See shaders/rt_reflection_spatial.comp.
    {
        std::array<VkDescriptorSetLayoutBinding, 4> sp_bindings{};
        for (uint32_t i = 0; i < 3; ++i) {
            sp_bindings[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                              VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        }
        sp_bindings[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

        // depth and mr are the render target's views and change on resize.
        std::array<VkDescriptorBindingFlags, 4> sp_flags{};
        sp_flags[1] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
        sp_flags[2] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;

        VkDescriptorSetLayoutBindingFlagsCreateInfo sp_flags_ci{};
        sp_flags_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        sp_flags_ci.bindingCount = static_cast<uint32_t>(sp_flags.size());
        sp_flags_ci.pBindingFlags = sp_flags.data();

        VkDescriptorSetLayoutCreateInfo sp_lci{};
        sp_lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        sp_lci.pNext = &sp_flags_ci;
        sp_lci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        sp_lci.bindingCount = static_cast<uint32_t>(sp_bindings.size());
        sp_lci.pBindings = sp_bindings.data();
        vk_check(vkCreateDescriptorSetLayout(vk_device_, &sp_lci, nullptr, &spatial_set_layout_),
                 "RT reflection spatial descriptor set layout");

        VkPushConstantRange sp_push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SpatialPush)};
        VkPipelineLayoutCreateInfo sp_pci{};
        sp_pci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        sp_pci.setLayoutCount = 1;
        sp_pci.pSetLayouts = &spatial_set_layout_;
        sp_pci.pushConstantRangeCount = 1;
        sp_pci.pPushConstantRanges = &sp_push;
        vk_check(vkCreatePipelineLayout(vk_device_, &sp_pci, nullptr, &spatial_pipeline_layout_),
                 "RT reflection spatial pipeline layout");

        auto sp_spirv = read_spirv(base_path::resolve("shaders/rt_reflection_spatial.comp.spv"));
        auto* sp_mod = create_shader_module(vk_device_, sp_spirv);
        VkPipelineShaderStageCreateInfo sp_stage{};
        sp_stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        sp_stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        sp_stage.module = sp_mod;
        sp_stage.pName = "main";
        VkComputePipelineCreateInfo sp_cci{};
        sp_cci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        sp_cci.stage = sp_stage;
        sp_cci.layout = spatial_pipeline_layout_;
        vk_check(vkCreateComputePipelines(vk_device_, resources.pipeline_cache, 1, &sp_cci,
                                          nullptr, &spatial_pipeline_),
                 "RT reflection spatial pipeline");
        vkDestroyShaderModule(vk_device_, sp_mod, nullptr);
    }

    // Per-viewport state (image + descriptor pool + UBO) is owned by
    // PostProcessResources, created once per viewport at the right size.

    FJELL_GFX_INFO("RT reflection pass created");
    return {};
}

// ── Images (per-viewport) ──────────────────────────────────────────────

void RtReflectionPass::Images::create(gpu::Device& gpu, VkDevice device, VmaAllocator allocator,
                                       VkDescriptorSetLayout set_layout,
                                       VkDescriptorSetLayout denoise_layout,
                                       VkDescriptorSetLayout spatial_layout,
                                       VkFormat depth_format,
                                       uint32_t width, uint32_t height) {
    out_extent = {width, height};
    out_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    first_use = true;
    history_valid = false;
    prev_depth_valid = false;

    auto make_color = [&](VkImage& image, VmaAllocation& alloc, VkImageView& view,
                          const char* what) {
        VkImageCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        ci.extent = {width, height, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_STORAGE_BIT
                 | VK_IMAGE_USAGE_SAMPLED_BIT
                 | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                 | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (create_vma_image(allocator, &ci, &image, &alloc) != VK_SUCCESS) {
            throw std::runtime_error(std::string("Failed to allocate RT reflection ") + what);
        }

        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device, &vi, nullptr, &view) != VK_SUCCESS) {
            throw std::runtime_error(std::string("Failed to create RT reflection ") + what + " view");
        }
    };

    make_color(out_image, out_alloc, out_view, "output image");
    make_color(raw_image, raw_alloc, raw_view, "raw image");
    VkImageCreateInfo color_info{};
    color_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    color_info.imageType = VK_IMAGE_TYPE_2D;
    color_info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    color_info.extent = {width, height, 1};
    color_info.mipLevels = 1;
    color_info.arrayLayers = 1;
    color_info.samples = VK_SAMPLE_COUNT_1_BIT;
    color_info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    out_texture = gpu::vulkan::adopt(gpu, out_image, out_view, color_info);
    raw_texture = gpu::vulkan::adopt(gpu, raw_image, raw_view, color_info);
    make_color(history_image, history_alloc, history_view, "history image");

    // Previous frame's depth, for the denoise's disocclusion test. R32F colour
    // rather than a depth format: the denoise writes it as a storage image, so
    // the shared depth buffer is only ever sampled, never transitioned by this
    // pass. It lives at the reflection's resolution, which is what the denoise
    // reprojects in.
    depth_extent = {width, height};
    prev_depth_slot = 0;
    for (size_t i = 0; i < prev_depth_image.size(); ++i) {
        VkImageCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_R32_SFLOAT;
        ci.extent = {width, height, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (create_vma_image(allocator, &ci, &prev_depth_image[i], &prev_depth_alloc[i])
                != VK_SUCCESS) {
            throw std::runtime_error("Failed to allocate RT reflection prev-depth image");
        }

        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = prev_depth_image[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R32_SFLOAT;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device, &vi, nullptr, &prev_depth_view[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create RT reflection prev-depth view");
        }
    }
    (void)depth_format;

    // Reflection UBO (sun light) — host-mapped, written each frame, one per
    // frame slot.
    for (auto& ubo : reflection_ubo) {
        ubo = std::make_unique<Buffer>(
            allocator, sizeof(ReflectionUbo),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
        ubo->map();
    }
    for (auto& ubo : denoise_ubo) {
        ubo = std::make_unique<Buffer>(
            allocator, sizeof(GpuReflectionDenoiseConstants),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
        ubo->map();
    }

    // Descriptor pool + set.
    // Sizes are per set; one trace set and one denoise set per frame slot.
    constexpr uint32_t SLOTS = MAX_FRAMES_IN_FLIGHT;
    std::array<VkDescriptorPoolSize, 5> pool_sizes{};
    // trace: DDGI UBO + reflection UBO; denoise: constants UBO
    pool_sizes[0] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,             3 * SLOTS};
    pool_sizes[1] = {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, SLOTS};
    // trace: depth, normal, mr, sky, irr, vis, probe_off (7)
    // denoise: raw, history, depth, prev_depth, mr (5)
    // + spatial: reflection_in, depth, mr (3)
    pool_sizes[2] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,     15 * SLOTS};
    // trace: reflection_out; denoise: resolved_out
    pool_sizes[3] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,              4 * SLOTS};
    pool_sizes[4] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             4 * SLOTS}; // vertex, index, instance, probe_status

    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pool_ci.maxSets = 3 * SLOTS;
    pool_ci.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
    pool_ci.pPoolSizes = pool_sizes.data();
    if (vkCreateDescriptorPool(device, &pool_ci, nullptr, &desc_pool) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create RT reflection descriptor pool");
    }

    std::array<VkDescriptorSetLayout, SLOTS> layouts{};
    layouts.fill(set_layout);
    VkDescriptorSetAllocateInfo set_alloc{};
    set_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    set_alloc.descriptorPool = desc_pool;
    set_alloc.descriptorSetCount = SLOTS;
    set_alloc.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(device, &set_alloc, trace_set.data()) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate RT reflection descriptor sets");
    }

    std::array<VkDescriptorSetLayout, SLOTS> denoise_layouts{};
    denoise_layouts.fill(denoise_layout);
    set_alloc.pSetLayouts = denoise_layouts.data();
    if (vkAllocateDescriptorSets(device, &set_alloc, denoise_set.data()) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate RT reflection denoise sets");
    }

    std::array<VkDescriptorSetLayout, SLOTS> spatial_layouts{};
    spatial_layouts.fill(spatial_layout);
    set_alloc.pSetLayouts = spatial_layouts.data();
    if (vkAllocateDescriptorSets(device, &set_alloc, spatial_set.data()) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate RT reflection spatial sets");
    }
}

void RtReflectionPass::Images::destroy(VkDevice device, VmaAllocator allocator) {
    if (desc_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, desc_pool, nullptr);
        desc_pool = VK_NULL_HANDLE;
        trace_set.fill(VK_NULL_HANDLE);
        denoise_set.fill(VK_NULL_HANDLE);
        spatial_set.fill(VK_NULL_HANDLE);
    }
    for (auto& ubo : reflection_ubo) ubo.reset();
    for (auto& ubo : denoise_ubo) ubo.reset();

    auto drop_view = [&](VkImageView& view) {
        if (view != VK_NULL_HANDLE) {
            vkDestroyImageView(device, view, nullptr);
            view = VK_NULL_HANDLE;
        }
    };
    auto drop_image = [&](VkImage& image, VmaAllocation& alloc) {
        if (image != VK_NULL_HANDLE) {
            destroy_vma_image(allocator, image, alloc);
            image = VK_NULL_HANDLE;
            alloc = VK_NULL_HANDLE;
        }
    };

    out_texture.reset();
    raw_texture.reset();
    drop_view(out_view);          drop_image(out_image, out_alloc);
    drop_view(raw_view);          drop_image(raw_image, raw_alloc);
    drop_view(history_view);      drop_image(history_image, history_alloc);
    for (size_t i = 0; i < prev_depth_image.size(); ++i) {
        drop_view(prev_depth_view[i]);
        drop_image(prev_depth_image[i], prev_depth_alloc[i]);
    }
    prev_depth_slot = 0;

    out_extent = {};
    depth_extent = {};
    out_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    first_use = true;
    history_valid = false;
    prev_depth_valid = false;
}

void RtReflectionPass::Images::resize(gpu::Device& gpu, VkDevice device, VmaAllocator allocator,
                                       VkDescriptorSetLayout set_layout,
                                       VkDescriptorSetLayout denoise_layout,
                                       VkDescriptorSetLayout spatial_layout,
                                       VkFormat depth_format,
                                       uint32_t width, uint32_t height) {
    if (out_extent.width == width && out_extent.height == height) return;
    vkDeviceWaitIdle(device);
    destroy(device, allocator);
    if (width > 0 && height > 0) {
        create(gpu, device, allocator, set_layout, denoise_layout, spatial_layout,
               depth_format, width, height);
    }
}

void RtReflectionPass::destroy() {
    if (vk_device_ == VK_NULL_HANDLE) return;


    if (spatial_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk_device_, spatial_pipeline_, nullptr);
        spatial_pipeline_ = VK_NULL_HANDLE;
    }
    if (spatial_pipeline_layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(vk_device_, spatial_pipeline_layout_, nullptr);
        spatial_pipeline_layout_ = VK_NULL_HANDLE;
    }
    if (spatial_set_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(vk_device_, spatial_set_layout_, nullptr);
        spatial_set_layout_ = VK_NULL_HANDLE;
    }
    if (denoise_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk_device_, denoise_pipeline_, nullptr);
        denoise_pipeline_ = VK_NULL_HANDLE;
    }
    if (denoise_pipeline_layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(vk_device_, denoise_pipeline_layout_, nullptr);
        denoise_pipeline_layout_ = VK_NULL_HANDLE;
    }
    if (denoise_set_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(vk_device_, denoise_set_layout_, nullptr);
        denoise_set_layout_ = VK_NULL_HANDLE;
    }
    if (trace_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk_device_, trace_pipeline_, nullptr);
        trace_pipeline_ = VK_NULL_HANDLE;
    }
    if (trace_pipeline_layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(vk_device_, trace_pipeline_layout_, nullptr);
        trace_pipeline_layout_ = VK_NULL_HANDLE;
    }
    if (trace_set_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(vk_device_, trace_set_layout_, nullptr);
        trace_set_layout_ = VK_NULL_HANDLE;
    }
    if (linear_sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(vk_device_, linear_sampler_, nullptr);
        linear_sampler_ = VK_NULL_HANDLE;
    }
    if (nearest_sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(vk_device_, nearest_sampler_, nullptr);
        nearest_sampler_ = VK_NULL_HANDLE;
    }
}

PassSetup RtReflectionPass::setup() const {
    return {
        .name = "rt_reflection",
        .phase = RenderPhase::PostProcess,
        // Sit alongside ssr_trace (sub_order 0) so ssr_composite (sub_order 1)
        // sees both producers. Only one writes per-frame depending on mode.
        .sub_order = 0,
    };
}

void RtReflectionPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    // The output image lives in PostProcessResources (one per viewport),
    // not the framegraph. We declare side-effects so the pass isn't
    // culled, and import the screen-space inputs the trace reads so the
    // graph brings them to the compute stage.
    builder.has_side_effects();

    if (ray_traced_active()) {
        AccelStructureManager::declare_trace(builder, ctx);
        auto depth = builder.import_named(ctx, "depth");
        auto normal = builder.import_named(ctx, "normal");
        auto mr = builder.import_named(ctx, "mr");
        builder.read(depth, gpu::Access::sampled_compute);
        builder.read(normal, gpu::Access::sampled_compute);
        builder.read(mr, gpu::Access::sampled_compute);
        // A reflection ray that misses reads the sky.
        if (auto sky = builder.import_named(ctx, "sky_cubemap"); sky.valid()) {
            builder.read(sky, gpu::Access::sampled_compute);
        }
    }
}

void RtReflectionPass::record(const FrameContext& ctx) {
    if (ctx.target == nullptr) return;
    if (ctx.pp_resources == nullptr) return;
    auto* imgs = ctx.pp_resources->rt_reflection();
    if (imgs == nullptr || imgs->out_image == VK_NULL_HANDLE) return;

    FJELL_PROFILE_SCOPE_N("record_rt_reflection");

    // Publish the per-viewport image to the registry so the composite
    // can find it. (PostProcessResources owns the image; the registry
    // entry just points at it for the consumer to read.)
    if (ctx.registry != nullptr) {
        ctx.registry->register_image("rt_reflection_output",
            {.frame_graph_id = 0,
             .texture = imgs->out_texture,
             .image = imgs->out_image,
             .view = imgs->out_view,
             .sampler = linear_sampler_});
    }

    // First-use seed: a fresh image starts in UNDEFINED. The composite
    // expects SHADER_READ unconditionally, but several early-outs below
    // (TLAS not ready, DDGI not allocated, etc.) can return before any
    // barrier runs. Seed the layout once so even an early-out leaves
    // the image in a valid layout for the composite.
    if (imgs->out_layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        emit_image_barrier(ctx.cmd, imgs->out_image,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        imgs->out_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    // ── Inactive path: clear and transition to SHADER_READ for composite ──
    if (!ray_traced_active()) {
        emit_image_barrier(ctx.cmd, imgs->out_image,
            imgs->out_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

        VkClearColorValue clear{};
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(ctx.cmd, imgs->out_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &clear, 1, &range);

        emit_image_barrier(ctx.cmd, imgs->out_image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        imgs->out_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        return;
    }

    // ── Active path: prerequisites ──
    // This frame's TLAS for the scene, built by the rt_scene pass; none when
    // no tracer's reach held anything.
    const VkAccelerationStructureKHR tlas =
        accel_manager_ != nullptr && ctx.scene != nullptr
            ? gpu::vulkan::native_acceleration(*ctx.device, accel_manager_->tlas(ctx.scene->id(), ctx.frame_index))
            : VK_NULL_HANDLE;
    if (tlas == VK_NULL_HANDLE) return;
    auto* ddgi_imgs = ctx.pp_resources->ddgi();
    if (ddgi_imgs == nullptr || ddgi_imgs->irradiance_view == VK_NULL_HANDLE) return;

    auto depth_view = ctx.target->depth_view();
    auto normal_view = ctx.target->normal_view();
    auto mr_view = ctx.target->mr_view();
    if (depth_view == VK_NULL_HANDLE || normal_view == VK_NULL_HANDLE || mr_view == VK_NULL_HANDLE) return;

    auto* sky_res = ctx.registry->find_image("sky_cubemap");
    if (sky_res == nullptr || sky_res->view == VK_NULL_HANDLE) return;

    if (ctx.mega_buffer == nullptr) return;

    FJELL_GPU_ZONE(ctx, "RT Reflection");

    // ── Update reflection UBO (sun light) ──
    {
        ReflectionUbo u{};
        u.sun_dir_intensity = glm::vec4(glm::normalize(-sun_direction_), sun_intensity_);
        u.sun_color = glm::vec4(sun_color_, 0.0F);
        std::memcpy(imgs->reflection_ubo[ctx.frame_index]->mapped(), &u, sizeof(u));
    }

    auto cmd = ctx.cmd;

    // ── Pre-trace: raw target to GENERAL ──
    // The trace writes raw_image; the denoise below reads it and writes
    // out_image, which is what the composite sees. Depth, normal and MR
    // arrive at the compute stage through the graph's barriers.
    emit_image_barrier(cmd, imgs->raw_image,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    // ── Update descriptor set ──
    // First-use writes: static bindings (vertex/index SSBOs, output, UBOs).
    if (imgs->first_use) {
        for (uint32_t s = 0; s < MAX_FRAMES_IN_FLIGHT; ++s) {
        std::array<VkWriteDescriptorSet, 3> static_writes{};
        for (auto& w : static_writes) {
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = imgs->trace_set[s];
            w.descriptorCount = 1;
        }

        VkDescriptorBufferInfo refl_ubo_info{imgs->reflection_ubo[s]->handle(), 0, sizeof(ReflectionUbo)};
        static_writes[0].dstBinding = 14;
        static_writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        static_writes[0].pBufferInfo = &refl_ubo_info;

        VkDescriptorBufferInfo vert_info{
            gpu::vulkan::native_buffer(*ctx.device, ctx.mega_buffer->vertex_buffer()), 0, VK_WHOLE_SIZE};
        static_writes[1].dstBinding = 7;
        static_writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        static_writes[1].pBufferInfo = &vert_info;

        VkDescriptorBufferInfo idx_info{
            gpu::vulkan::native_buffer(*ctx.device, ctx.mega_buffer->index_buffer()), 0, VK_WHOLE_SIZE};
        static_writes[2].dstBinding = 8;
        static_writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        static_writes[2].pBufferInfo = &idx_info;

        vkUpdateDescriptorSets(vk_device_, static_cast<uint32_t>(static_writes.size()),
                               static_writes.data(), 0, nullptr);
        }
        imgs->first_use = false;
    }

    // Per-frame writes: TLAS + screen images + DDGI atlases + storage output.
    {
        // Reflection storage output (our own image).
        VkDescriptorImageInfo out_info{};
        out_info.imageView = imgs->raw_view;
        out_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        // Screen-space samplers.
        VkDescriptorImageInfo depth_info{nearest_sampler_, depth_view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo normal_info{nearest_sampler_, normal_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo mr_info{linear_sampler_, mr_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        // Use the cubemap's own sampler (set by SkyCubemapPass) — it's
        // configured for cubemap sampling, not generic 2D.
        VkSampler sky_sampler = (sky_res->sampler != VK_NULL_HANDLE) ? sky_res->sampler : linear_sampler_;
        VkDescriptorImageInfo sky_info{sky_sampler, sky_res->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

        // DDGI atlases.
        VkDescriptorImageInfo irr_info{linear_sampler_, ddgi_imgs->irradiance_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo vis_info{linear_sampler_, ddgi_imgs->visibility_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo offset_info{nearest_sampler_, ddgi_imgs->probe_offset_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

        // DDGI UBO (binding 0) — share buffer with DdgiPass so the layout
        // matches the trace's struct without us replicating the data.
        VkDescriptorBufferInfo ddgi_ubo_info{ddgi_imgs->constants_ubo[ctx.frame_index]->handle(), 0, VK_WHOLE_SIZE};

        // Probe status SSBO.
        VkDescriptorBufferInfo probe_status_info{ddgi_imgs->probe_status_buffer->handle(), 0, VK_WHOLE_SIZE};

        // RT instance data SSBO.
        VkDescriptorBufferInfo inst_info{
            gpu::vulkan::native_buffer(*ctx.device, accel_manager_->instance_data_buffer(ctx.scene->id(), ctx.frame_index)), 0, VK_WHOLE_SIZE};

        // TLAS.
        VkWriteDescriptorSetAccelerationStructureKHR tlas_as{};
        tlas_as.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
        tlas_as.accelerationStructureCount = 1;
        tlas_as.pAccelerationStructures = &tlas;

        std::array<VkWriteDescriptorSet, 12> w{};
        for (auto& wi : w) {
            wi.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wi.dstSet = imgs->trace_set[ctx.frame_index];
            wi.descriptorCount = 1;
        }
        w[0].dstBinding = 0;  w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;             w[0].pBufferInfo = &ddgi_ubo_info;
        w[1].dstBinding = 1;  w[1].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR; w[1].pNext = &tlas_as;
        w[2].dstBinding = 2;  w[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;     w[2].pImageInfo = &depth_info;
        w[3].dstBinding = 3;  w[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;     w[3].pImageInfo = &normal_info;
        w[4].dstBinding = 4;  w[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;     w[4].pImageInfo = &mr_info;
        w[5].dstBinding = 5;  w[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;     w[5].pImageInfo = &sky_info;
        w[6].dstBinding = 6;  w[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;              w[6].pImageInfo = &out_info;
        w[7].dstBinding = 9;  w[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;             w[7].pBufferInfo = &inst_info;
        w[8].dstBinding = 10; w[8].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;     w[8].pImageInfo = &irr_info;
        w[9].dstBinding = 11; w[9].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;     w[9].pImageInfo = &vis_info;
        w[10].dstBinding = 12; w[10].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;   w[10].pImageInfo = &offset_info;
        w[11].dstBinding = 13; w[11].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;           w[11].pBufferInfo = &probe_status_info;

        vkUpdateDescriptorSets(vk_device_, static_cast<uint32_t>(w.size()), w.data(), 0, nullptr);
    }

    // ── Bind + dispatch ──
    std::array<VkDescriptorSet, 2> sets = {imgs->trace_set[ctx.frame_index], ctx.bindless_set};
    ReflectionPush push{};
    push.frame_index = frame_counter_++;
    push.max_trace_dist = max_trace_dist_;
    push.trace_cell_size = trace_cell_size_;
    push.intensity = intensity_;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, trace_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            trace_pipeline_layout_, 0,
                            static_cast<uint32_t>(sets.size()), sets.data(),
                            0, nullptr);
    vkCmdPushConstants(cmd, trace_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);

    // One invocation per pixel.
    vkCmdDispatch(cmd,
                  (imgs->out_extent.width + 7) / 8,
                  (imgs->out_extent.height + 7) / 8, 1);

    // ── Temporal denoise ──
    // FJELL_DISABLE_RT_DENOISE=1 skips accumulation and hands the composite the
    // raw trace, for A/B against the denoised image.
    static const bool denoise_disabled = [] {
        const char* v = std::getenv("FJELL_DISABLE_RT_DENOISE");
        return v != nullptr && v[0] == '1';
    }();

    if (denoise_disabled) {
        emit_image_barrier(cmd, imgs->raw_image,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        emit_image_barrier(cmd, imgs->out_image,
            imgs->out_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageCopy raw_copy{};
        raw_copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        raw_copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        raw_copy.extent = {imgs->out_extent.width, imgs->out_extent.height, 1};
        vkCmdCopyImage(cmd, imgs->raw_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       imgs->out_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &raw_copy);
        emit_image_barrier(cmd, imgs->out_image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        imgs->out_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        return;
    }

    record_denoise(ctx, *imgs);
}

void RtReflectionPass::record_denoise(const FrameContext& ctx, Images& imgs) {
    auto cmd = ctx.cmd;

    FJELL_GPU_ZONE(ctx, "RT Reflection Denoise");

    // Raw trace output becomes a sampled input.
    emit_image_barrier(cmd, imgs.raw_image,
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    // Resolved target to GENERAL for the compute write.
    emit_image_barrier(cmd, imgs.out_image,
        imgs.out_layout, VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    // History: sampled. On the first frame it holds nothing, and the shader
    // rejects it while frame_index < 2 — the barrier only makes the layout legal.
    emit_image_barrier(cmd, imgs.history_image,
        imgs.history_valid ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    // Ping-pong: read what the previous dispatch wrote, write the other slot.
    const uint32_t read_slot = imgs.prev_depth_slot;
    const uint32_t write_slot = 1U - read_slot;

    emit_image_barrier(cmd, imgs.prev_depth_image[read_slot],
        imgs.prev_depth_valid ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    emit_image_barrier(cmd, imgs.prev_depth_image[write_slot],
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    // ── Constants ──
    {
        GpuReflectionDenoiseConstants c{};
        c.projection = ctx.projection;
        c.inv_view_proj = ctx.inv_view_proj;
        c.prev_view_proj = imgs.prev_view_proj;
        c.prev_inv_view_proj = imgs.prev_inv_view_proj;
        c.camera_position = ctx.camera_position;
        std::memcpy(imgs.denoise_ubo[ctx.frame_index]->mapped(), &c, sizeof(c));
    }

    // ── Descriptors ──
    {
        VkDescriptorImageInfo raw_info{linear_sampler_, imgs.raw_view,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo hist_info{linear_sampler_, imgs.history_view,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo depth_info{nearest_sampler_, ctx.target->depth_view(),
                                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo pdepth_info{nearest_sampler_, imgs.prev_depth_view[read_slot],
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo pdepth_out_info{VK_NULL_HANDLE, imgs.prev_depth_view[write_slot],
                                              VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo mr_info{nearest_sampler_, ctx.target->mr_view(),
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo out_info{VK_NULL_HANDLE, imgs.out_view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorBufferInfo ubo_info{imgs.denoise_ubo[ctx.frame_index]->handle(), 0,
                                        sizeof(GpuReflectionDenoiseConstants)};

        std::array<VkWriteDescriptorSet, 8> w{};
        for (auto& write : w) {
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = imgs.denoise_set[ctx.frame_index];
            write.descriptorCount = 1;
        }
        w[0].dstBinding = 0; w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &raw_info;
        w[1].dstBinding = 1; w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[1].pImageInfo = &hist_info;
        w[2].dstBinding = 2; w[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[2].pImageInfo = &depth_info;
        w[3].dstBinding = 3; w[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[3].pImageInfo = &pdepth_info;
        w[4].dstBinding = 4; w[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[4].pImageInfo = &mr_info;
        w[5].dstBinding = 5; w[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;          w[5].pImageInfo = &out_info;
        w[6].dstBinding = 6; w[6].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;         w[6].pBufferInfo = &ubo_info;
        w[7].dstBinding = 7; w[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;          w[7].pImageInfo = &pdepth_out_info;
        vkUpdateDescriptorSets(vk_device_, static_cast<uint32_t>(w.size()), w.data(), 0, nullptr);
    }

    // ── Dispatch ──
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, denoise_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, denoise_pipeline_layout_,
                            0, 1, &imgs.denoise_set[ctx.frame_index], 0, nullptr);

    DenoisePush push{};
    push.width = imgs.out_extent.width;
    push.height = imgs.out_extent.height;
    push.frame_index = frame_counter_;
    push.hysteresis = (imgs.history_valid && imgs.prev_depth_valid) ? hysteresis_ : 0.0F;
    // FJELL_RT_DENOISE_DEBUG=1|2|3 replaces the reflection with a diagnostic
    // view: surviving blend, which test rejected history, or how hard the
    // neighbourhood clamp bit. See rt_reflection_denoise.comp.
    static const uint32_t debug_mode = [] {
        const char* v = std::getenv("FJELL_RT_DENOISE_DEBUG");
        return v != nullptr ? static_cast<uint32_t>(std::atoi(v)) : 0U;
    }();
    push.debug_mode = debug_mode;
    vkCmdPushConstants(cmd, denoise_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);

    vkCmdDispatch(cmd,
                  (imgs.out_extent.width + 7) / 8,
                  (imgs.out_extent.height + 7) / 8, 1);

    // ── Copy resolved → history for next frame ──
    emit_image_barrier(cmd, imgs.out_image,
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    emit_image_barrier(cmd, imgs.history_image,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.extent = {imgs.out_extent.width, imgs.out_extent.height, 1};
    vkCmdCopyImage(cmd,
                   imgs.out_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   imgs.history_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &copy);

    // The dispatch wrote this frame's depth into the write slot; next frame
    // reads it and writes the other one. The shared depth buffer was only
    // sampled, never transitioned, so nothing downstream of this pass is
    // disturbed.
    imgs.prev_depth_slot = write_slot;
    imgs.prev_depth_valid = true;

    // ── Resolved → SHADER_READ for the composite ──
    emit_image_barrier(cmd, imgs.out_image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    imgs.out_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // The denoise has consumed prev_view_proj; the history it just wrote
    // belongs to this frame's camera, so that becomes next frame's reference.
    imgs.prev_view_proj = glm::inverse(ctx.inv_view_proj);
    imgs.prev_inv_view_proj = ctx.inv_view_proj;
    imgs.history_valid = true;

    // Runs last: it samples out_image, which only reaches SHADER_READ_ONLY
    // above, after the history copy has released it from TRANSFER_SRC.
    record_spatial(ctx, imgs);
}

void RtReflectionPass::record_spatial(const FrameContext& ctx, Images& imgs) {
    if (spatial_strength_ <= 0.0F) return;

    auto cmd = ctx.cmd;
    FJELL_GPU_ZONE(ctx, "RT Reflection Spatial");

    // Filters the accumulated result into raw_image, which is free by now —
    // the trace's output was consumed by the denoise. What history keeps is the
    // *unfiltered* accumulation, so the neighbourhood clamp keeps measuring the
    // real sample spread rather than one this pass already smoothed.
    emit_image_barrier(cmd, imgs.raw_image,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    {
        VkDescriptorImageInfo in_info{linear_sampler_, imgs.out_view,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo depth_info{nearest_sampler_, ctx.target->depth_view(),
                                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo mr_info{nearest_sampler_, ctx.target->mr_view(),
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo out_info{VK_NULL_HANDLE, imgs.raw_view, VK_IMAGE_LAYOUT_GENERAL};

        std::array<VkWriteDescriptorSet, 4> w{};
        for (auto& write : w) {
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = imgs.spatial_set[ctx.frame_index];
            write.descriptorCount = 1;
        }
        w[0].dstBinding = 0; w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &in_info;
        w[1].dstBinding = 1; w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[1].pImageInfo = &depth_info;
        w[2].dstBinding = 2; w[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[2].pImageInfo = &mr_info;
        w[3].dstBinding = 3; w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;          w[3].pImageInfo = &out_info;
        vkUpdateDescriptorSets(vk_device_, static_cast<uint32_t>(w.size()), w.data(), 0, nullptr);
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, spatial_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, spatial_pipeline_layout_,
                            0, 1, &imgs.spatial_set[ctx.frame_index], 0, nullptr);

    SpatialPush push{};
    push.width = imgs.out_extent.width;
    push.height = imgs.out_extent.height;
    push.strength = spatial_strength_;
    push.depth_sigma = 0.01F;
    vkCmdPushConstants(cmd, spatial_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);

    vkCmdDispatch(cmd,
                  (imgs.out_extent.width + 7) / 8,
                  (imgs.out_extent.height + 7) / 8, 1);

    emit_image_barrier(cmd, imgs.raw_image,
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    // The composite reads the filtered image, not the accumulated one.
    if (ctx.registry != nullptr) {
        ctx.registry->register_image("rt_reflection_output",
            {.frame_graph_id = 0,
             .texture = imgs.raw_texture,
             .image = imgs.raw_image,
             .view = imgs.raw_view,
             .sampler = linear_sampler_});
    }
}

} // namespace fjell
