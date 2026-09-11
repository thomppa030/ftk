#include "renderer/pass/ss_gi_pass.hpp"
#include "core/profiler.hpp"
#include "renderer/pass/pass_gpu_zone.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/gpu/vma_image.hpp"
#include "renderer/gpu/accel_structure.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/resources/shader_utils.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/post_process_resources.hpp"
#include "renderer/frame_graph.hpp"
#include "renderer/resources/mega_buffer.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>

#include <cstring>

namespace fjell {

// ── Images lifecycle ────────────────────────────────────────────────────

void SsGiPass::Images::create(VkDevice device, VmaAllocator allocator,
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
    make_image(half_w, half_h,
               VK_FORMAT_R16G16B16A16_SFLOAT, storage_sampled,
               indirect_image, indirect_memory, indirect_view);

    // Constants UBO
    constants_ubo = std::make_unique<Buffer>(
        allocator, sizeof(GpuSsGiConstants),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    constants_ubo->map();

    first_frame = true;
}

void SsGiPass::Images::resize(VkDevice device, VmaAllocator allocator,
                               const SsGiConfig& config,
                               uint32_t screen_w, uint32_t screen_h) {
    if (indirect_image == VK_NULL_HANDLE) {
        create(device, allocator, config, screen_w, screen_h);
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
    create(device, allocator, config, screen_w, screen_h);
}

void SsGiPass::Images::destroy(VkDevice device, VmaAllocator allocator) {
    auto safe_view = [&](VkImageView& v) { if (v) { vkDestroyImageView(device, v, nullptr); v = VK_NULL_HANDLE; } };
    auto safe_img = [&](VkImage& i, VmaAllocation& a) { destroy_vma_image(allocator, i, a); i = VK_NULL_HANDLE; a = VK_NULL_HANDLE; };

    safe_view(raw_radiance_view); safe_img(raw_radiance_image, raw_radiance_memory);
    safe_view(resolved_view); safe_img(resolved_image, resolved_memory);
    safe_view(history_view); safe_img(history_image, history_memory);
    safe_view(indirect_view); safe_img(indirect_image, indirect_memory);
    safe_view(prev_depth_view); safe_img(prev_depth_image, prev_depth_memory);
    constants_ubo.reset();

    if (desc_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, desc_pool, nullptr);
        desc_pool = VK_NULL_HANDLE;
    }
    trace_set = VK_NULL_HANDLE;
    resolve_set = VK_NULL_HANDLE;
    upsample_set = VK_NULL_HANDLE;

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

    // ── RT trace pipeline ──
    if (device.ray_tracing_supported()) {
        // Descriptor set layout for trace:
        // 0: UBO, 1: TLAS, 2: depth, 3: normal, 5: radiance_out
        // 6: vertex SSBO, 7: index SSBO, 8: instance data SSBO
        // Binding 4 is unused: it held a previous-frame color image the trace
        // never sampled. The remaining binding numbers match the shader.
        std::array<VkDescriptorSetLayoutBinding, 8> trace_bindings{};
        VkShaderStageFlags all_rt = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR;
        trace_bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, all_rt, nullptr};
        trace_bindings[1] = {1, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1,
                             VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr};
        trace_bindings[2] = {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                             VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr};
        trace_bindings[3] = {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                             VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr};
        trace_bindings[4] = {5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                             VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr};
        trace_bindings[5] = {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                             VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, nullptr};
        trace_bindings[6] = {7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                             VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, nullptr};
        trace_bindings[7] = {8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                             VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, nullptr};

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
        push.stageFlags = all_rt;
        push.size = sizeof(TracePushConstants);

        std::array trace_layouts = {trace_set_layout_, resources.material_set_layout};
        VkPipelineLayoutCreateInfo pipe_ci{};
        pipe_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipe_ci.setLayoutCount = static_cast<uint32_t>(trace_layouts.size());
        pipe_ci.pSetLayouts = trace_layouts.data();
        pipe_ci.pushConstantRangeCount = 1;
        pipe_ci.pPushConstantRanges = &push;
        vkCreatePipelineLayout(vk_device_, &pipe_ci, nullptr, &trace_pipeline_layout_);

        // Load RT shaders
        auto rgen_spirv = read_spirv(base_path::resolve("shaders/ss_gi_trace.rgen.spv"));
        auto rchit_spirv = read_spirv(base_path::resolve("shaders/ss_gi_trace.rchit.spv"));
        auto rmiss_spirv = read_spirv(base_path::resolve("shaders/ss_gi_trace.rmiss.spv"));

        auto rgen_mod = create_shader_module(vk_device_, rgen_spirv);
        auto rchit_mod = create_shader_module(vk_device_, rchit_spirv);
        auto rmiss_mod = create_shader_module(vk_device_, rmiss_spirv);

        std::array<VkPipelineShaderStageCreateInfo, 3> stages{};
        for (auto& s : stages) s.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR; stages[0].module = rgen_mod; stages[0].pName = "main";
        stages[1].stage = VK_SHADER_STAGE_MISS_BIT_KHR;   stages[1].module = rmiss_mod; stages[1].pName = "main";
        stages[2].stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR; stages[2].module = rchit_mod; stages[2].pName = "main";

        std::array<VkRayTracingShaderGroupCreateInfoKHR, 3> groups{};
        for (auto& g : groups) {
            g.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
            g.generalShader = VK_SHADER_UNUSED_KHR;
            g.closestHitShader = VK_SHADER_UNUSED_KHR;
            g.anyHitShader = VK_SHADER_UNUSED_KHR;
            g.intersectionShader = VK_SHADER_UNUSED_KHR;
        }
        groups[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        groups[0].generalShader = 0;
        groups[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        groups[1].generalShader = 1;
        groups[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
        groups[2].closestHitShader = 2;

        VkRayTracingPipelineCreateInfoKHR rt_ci{};
        rt_ci.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
        rt_ci.stageCount = static_cast<uint32_t>(stages.size());
        rt_ci.pStages = stages.data();
        rt_ci.groupCount = static_cast<uint32_t>(groups.size());
        rt_ci.pGroups = groups.data();
        rt_ci.maxPipelineRayRecursionDepth = 1;
        rt_ci.layout = trace_pipeline_layout_;

        VkResult rt_result = device.create_rt_pipelines_fn()(
            vk_device_, VK_NULL_HANDLE, resources.pipeline_cache, 1, &rt_ci, nullptr, &trace_pipeline_);

        vkDestroyShaderModule(vk_device_, rgen_mod, nullptr);
        vkDestroyShaderModule(vk_device_, rchit_mod, nullptr);
        vkDestroyShaderModule(vk_device_, rmiss_mod, nullptr);

        if (rt_result != VK_SUCCESS) {
            FJELL_GFX_WARN("Failed to create SS-GI RT pipeline");
        } else {
            // Build SBT
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
                vk_device_, trace_pipeline_, 0, group_count,
                handles.size(), handles.data());

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
            if (!sbt_data) { sbt_buffer_->map(); sbt_data = static_cast<uint8_t*>(sbt_buffer_->mapped()); }

            std::memcpy(sbt_data, handles.data(), handle_size);
            std::memcpy(sbt_data + raygen_size, handles.data() + handle_size, handle_size);
            std::memcpy(sbt_data + raygen_size + miss_size, handles.data() + 2 * handle_size, handle_size);

            VkBufferDeviceAddressInfo addr_info{};
            addr_info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
            addr_info.buffer = sbt_buffer_->handle();
            VkDeviceAddress sbt_addr = device.get_buffer_device_address_fn()(vk_device_, &addr_info);

            sbt_raygen_ = {sbt_addr, raygen_size, raygen_size};
            sbt_miss_ = {sbt_addr + raygen_size, miss_size, miss_size};
            sbt_hit_ = {sbt_addr + raygen_size + miss_size, hit_size, hit_size};
            sbt_callable_ = {};

            FJELL_GFX_INFO("SS-GI RT pipeline created");
        }
    }

    // ── Resolve compute pipeline ──
    {
        std::array<VkDescriptorSetLayoutBinding, 6> bindings{};
        for (uint32_t i = 0; i < 5; ++i)
            bindings[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[5] = {5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

        std::array<VkDescriptorBindingFlags, 6> resolve_flags{};
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
    sbt_buffer_.reset();
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

    constants.prev_view_proj = prev_view_proj_;
    constants.camera_position = ctx.camera_position;
    constants.max_trace_dist = config_.max_trace_dist;
    // Resolution must match what the SS-GI images were created with (viewport size),
    // not the offscreen render target size (which may be the full window).
    constants.resolution = glm::vec2(imgs.indirect_extent.width * 2,
                                     imgs.indirect_extent.height * 2);
    constants.probe_spacing = config_.probe_spacing;
    constants.frame_index = frame_counter_;
    constants.probe_count = imgs.probe_count;

    // Sun light for closest-hit direct lighting (cached via set_sun_light)
    constants.sun_direction_intensity = glm::vec4(
        glm::normalize(-sun_direction_), sun_intensity_);
    constants.sun_color = glm::vec4(sun_color_, 0.0f);

    std::memcpy(imgs.constants_ubo->mapped(), &constants, sizeof(constants));

    // Store current frame's VP for next frame's prev_view_proj.
    // ctx.inv_view_proj is the correct jittered matrix from the GlobalUBO;
    // ctx.view/projection may be identity if the FrameContext wasn't fully populated.
    prev_view_proj_ = glm::inverse(ctx.inv_view_proj);
}

void SsGiPass::record(const FrameContext& ctx) {
    if (!config_.enabled || trace_pipeline_ == VK_NULL_HANDLE) { return; }
    auto* imgs = ctx.pp_resources ? ctx.pp_resources->ss_gi() : nullptr;
    if (imgs == nullptr) { return; }
    FJELL_PROFILE_SCOPE_N("record_ss_gi");
    FJELL_GPU_ZONE(ctx, "SS-GI");
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
    builder.read(depth, ResourceAccess::sampled_compute);
    builder.read(normal, ResourceAccess::sampled_compute);
    // Writes pass-private gi_indirect / ss_gi_emissive bound through
    // the global descriptor — consumer edge lives outside the catalog.
    builder.has_side_effects();
}

// ── Record ──────────────────────────────────────────────────────────────

void SsGiPass::record_into(const FrameContext& ctx, Images& imgs) {
    if (trace_pipeline_ == VK_NULL_HANDLE) return;
    if (imgs.raw_radiance_image == VK_NULL_HANDLE) return;
    if (!accel_manager_ || !accel_manager_->tlas_valid(ctx.frame_index)) return;
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
        std::array<VkDescriptorPoolSize, 4> pool_sizes{};
        pool_sizes[0] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1};
        pool_sizes[1] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 14};
        pool_sizes[2] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3};
        pool_sizes[3] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};

        // Need TLAS descriptor type too
        std::array<VkDescriptorPoolSize, 5> all_sizes{};
        all_sizes[0] = pool_sizes[0];
        all_sizes[1] = pool_sizes[1];
        all_sizes[2] = pool_sizes[2];
        all_sizes[3] = pool_sizes[3];
        all_sizes[4] = {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1};

        VkDescriptorPoolCreateInfo pool_ci{};
        pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_ci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        pool_ci.maxSets = 3;
        pool_ci.poolSizeCount = 5;
        pool_ci.pPoolSizes = all_sizes.data();
        vkCreateDescriptorPool(vk_device_, &pool_ci, nullptr, &imgs.desc_pool);

        std::array layouts = {trace_set_layout_, resolve_set_layout_, upsample_set_layout_};
        std::array<VkDescriptorSet, 3> sets{};
        VkDescriptorSetAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = imgs.desc_pool;
        alloc_info.descriptorSetCount = 3;
        alloc_info.pSetLayouts = layouts.data();
        vkAllocateDescriptorSets(vk_device_, &alloc_info, sets.data());
        imgs.trace_set = sets[0];
        imgs.resolve_set = sets[1];
        imgs.upsample_set = sets[2];
        imgs.first_frame = true;
    }

    // ── Write static descriptors once ──
    if (imgs.first_frame) {
        // Static bindings that don't change per-frame
        VkDescriptorBufferInfo ubo_info{imgs.constants_ubo->handle(), 0, sizeof(GpuSsGiConstants)};
        VkDescriptorImageInfo radiance_out_info{};
        radiance_out_info.imageView = imgs.raw_radiance_view;
        radiance_out_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        std::array<VkWriteDescriptorSet, 2> sw{};
        for (auto& w : sw) { w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = imgs.trace_set; w.descriptorCount = 1; }
        sw[0].dstBinding = 0; sw[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; sw[0].pBufferInfo = &ubo_info;
        sw[1].dstBinding = 5; sw[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; sw[1].pImageInfo = &radiance_out_info;
        vkUpdateDescriptorSets(vk_device_, static_cast<uint32_t>(sw.size()), sw.data(), 0, nullptr);

        if (ctx.mega_buffer) {
            VkDescriptorBufferInfo vert_info{ctx.mega_buffer->vertex_buffer_handle(), 0, VK_WHOLE_SIZE};
            VkDescriptorBufferInfo idx_info{ctx.mega_buffer->index_buffer_handle(), 0, VK_WHOLE_SIZE};
            std::array<VkWriteDescriptorSet, 2> bw{};
            for (auto& w : bw) { w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = imgs.trace_set; w.descriptorCount = 1; }
            bw[0].dstBinding = 6; bw[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; bw[0].pBufferInfo = &vert_info;
            bw[1].dstBinding = 7; bw[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; bw[1].pBufferInfo = &idx_info;
            vkUpdateDescriptorSets(vk_device_, static_cast<uint32_t>(bw.size()), bw.data(), 0, nullptr);
        }

        // Resolve set: static bindings (raw radiance, history, resolved output)
        VkDescriptorImageInfo raw_info{nearest_sampler_, imgs.raw_radiance_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo hist_info{linear_sampler_, imgs.history_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo resolve_out_info{};
        resolve_out_info.imageView = imgs.resolved_view;
        resolve_out_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        std::array<VkWriteDescriptorSet, 3> rsw{};
        for (auto& w : rsw) { w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = imgs.resolve_set; w.descriptorCount = 1; }
        rsw[0].dstBinding = 0; rsw[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; rsw[0].pImageInfo = &raw_info;
        rsw[1].dstBinding = 1; rsw[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; rsw[1].pImageInfo = &hist_info;
        rsw[2].dstBinding = 5; rsw[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; rsw[2].pImageInfo = &resolve_out_info;
        vkUpdateDescriptorSets(vk_device_, static_cast<uint32_t>(rsw.size()), rsw.data(), 0, nullptr);

        // Upsample set: static bindings (resolved probes, indirect output)
        VkDescriptorImageInfo resolved_info{linear_sampler_, imgs.resolved_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo indirect_out_info{};
        indirect_out_info.imageView = imgs.indirect_view;
        indirect_out_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        std::array<VkWriteDescriptorSet, 2> usw{};
        for (auto& w : usw) { w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = imgs.upsample_set; w.descriptorCount = 1; }
        usw[0].dstBinding = 0; usw[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; usw[0].pImageInfo = &resolved_info;
        usw[1].dstBinding = 3; usw[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; usw[1].pImageInfo = &indirect_out_info;
        vkUpdateDescriptorSets(vk_device_, static_cast<uint32_t>(usw.size()), usw.data(), 0, nullptr);

        imgs.first_frame = false;
    }

    // ── Update per-frame descriptors (depth, normal, prev_color change each frame) ──
    {
        // Trace set: depth, normal
        VkDescriptorImageInfo di{nearest_sampler_, depth_view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo ni{nearest_sampler_, normal_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

        std::array<VkWriteDescriptorSet, 2> tw{};
        for (auto& w : tw) { w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = imgs.trace_set; w.descriptorCount = 1; }
        tw[0].dstBinding = 2; tw[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; tw[0].pImageInfo = &di;
        tw[1].dstBinding = 3; tw[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; tw[1].pImageInfo = &ni;
        vkUpdateDescriptorSets(vk_device_, static_cast<uint32_t>(tw.size()), tw.data(), 0, nullptr);

        // Resolve set: depth, normal, prev_depth (previous frame's depth copy)
        VkDescriptorImageInfo pdi{nearest_sampler_, imgs.prev_depth_view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        std::array<VkWriteDescriptorSet, 3> rw{};
        for (auto& w : rw) { w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = imgs.resolve_set; w.descriptorCount = 1; }
        rw[0].dstBinding = 2; rw[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; rw[0].pImageInfo = &di;
        rw[1].dstBinding = 3; rw[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; rw[1].pImageInfo = &ni;
        rw[2].dstBinding = 4; rw[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; rw[2].pImageInfo = &pdi;
        vkUpdateDescriptorSets(vk_device_, static_cast<uint32_t>(rw.size()), rw.data(), 0, nullptr);

        // Upsample set: depth, normal
        std::array<VkWriteDescriptorSet, 2> uw{};
        for (auto& w : uw) { w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = imgs.upsample_set; w.descriptorCount = 1; }
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

    // ── 1. Pre-trace barriers ──
    barrier_image(imgs.raw_radiance_image,
                  VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
                  VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_SHADER_WRITE_BIT);

    // Depth buffer: the frame graph transitions depth to READ_ONLY for DDGI's
    // compute_read, but the stage mask only covers COMPUTE_SHADER_BIT. We need
    // an execution barrier to extend visibility to RAY_TRACING_SHADER_BIT_KHR.
    // Same layout on both sides = pure execution/memory dependency, no transition.
    barrier_image(ctx.target->depth_image(),
                  VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                  VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_READ_BIT,
                  VK_IMAGE_ASPECT_DEPTH_BIT);

    // Normal buffer: same pattern — extend visibility to RT stage
    barrier_image(ctx.target->normal_image(),
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_READ_BIT);

    // ── 2. Update TLAS + instance data descriptors (per-frame) ──
    {
        VkWriteDescriptorSetAccelerationStructureKHR tlas_write_as{};
        tlas_write_as.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
        tlas_write_as.accelerationStructureCount = 1;
        VkAccelerationStructureKHR tlas = accel_manager_->tlas(ctx.frame_index);
        tlas_write_as.pAccelerationStructures = &tlas;

        VkWriteDescriptorSet tlas_write{};
        tlas_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        tlas_write.dstSet = imgs.trace_set;
        tlas_write.dstBinding = 1;
        tlas_write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        tlas_write.descriptorCount = 1;
        tlas_write.pNext = &tlas_write_as;

        VkDescriptorBufferInfo inst_info{accel_manager_->instance_data_buffer(ctx.frame_index), 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet inst_write{};
        inst_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        inst_write.dstSet = imgs.trace_set;
        inst_write.dstBinding = 8;
        inst_write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        inst_write.descriptorCount = 1;
        inst_write.pBufferInfo = &inst_info;

        std::array writes = {tlas_write, inst_write};
        vkUpdateDescriptorSets(vk_device_, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }

    // ── 3. Dispatch RT trace ──
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, trace_pipeline_);
        std::array rt_sets = {imgs.trace_set, ctx.bindless_set};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                                trace_pipeline_layout_, 0,
                                static_cast<uint32_t>(rt_sets.size()), rt_sets.data(),
                                0, nullptr);

        TracePushConstants push{};
        push.rays_per_probe = config_.rays_per_probe;
        push.ray_offset = frame_counter_ * config_.rays_per_probe;
        vkCmdPushConstants(cmd, trace_pipeline_layout_,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR,
                           0, sizeof(push), &push);

        // Dispatch: X = probe_count.x, Y = probe_count.y, Z = rays_per_probe
        device_->cmd_trace_rays_fn()(cmd,
                                     &sbt_raygen_, &sbt_miss_, &sbt_hit_, &sbt_callable_,
                                     imgs.probe_count.x, imgs.probe_count.y, config_.rays_per_probe);
    }

    // ── 4. Barrier: raw radiance write → read ──
    barrier_image(imgs.raw_radiance_image,
                  VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);

    // Resolved output to GENERAL for write
    barrier_image(imgs.resolved_image,
                  VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);

    // History to SHADER_READ_ONLY for sampling
    barrier_image(imgs.history_image,
                  VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);

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
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, resolve_pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                resolve_pipeline_layout_, 0, 1, &imgs.resolve_set, 0, nullptr);

        ResolvePushConstants push{};
        push.probe_count = imgs.probe_count;
        push.rays_per_probe = config_.rays_per_probe;
        push.frame_index = frame_counter_;
        push.resolution = glm::vec2(imgs.indirect_extent.width * 2,
                                    imgs.indirect_extent.height * 2);
        push.probe_spacing = config_.probe_spacing;
        push.hysteresis = config_.hysteresis;
        vkCmdPushConstants(cmd, resolve_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(push), &push);

        vkCmdDispatch(cmd,
                      (imgs.probe_count.x + 7) / 8,
                      (imgs.probe_count.y + 7) / 8, 1);
    }

    // ── 6. Copy resolved → history for next frame ──
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

    // ── 6b. Copy this frame's depth into prev_depth for next frame ──
    // The resolve above has already read prev_depth (last frame's copy), so it
    // is now safe to overwrite. Next frame's resolve compares against this.
    barrier_image(ctx.target->depth_image(),
                  VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                  VK_IMAGE_ASPECT_DEPTH_BIT);
    barrier_image(imgs.prev_depth_image,
                  imgs.prev_depth_valid ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_IMAGE_ASPECT_DEPTH_BIT);

    VkImageCopy depth_copy{};
    depth_copy.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    depth_copy.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    depth_copy.extent = {imgs.depth_extent.width, imgs.depth_extent.height, 1};
    vkCmdCopyImage(cmd,
                   ctx.target->depth_image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   imgs.prev_depth_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &depth_copy);

    // Restore both depth images to their sampled layout.
    barrier_image(ctx.target->depth_image(),
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                  VK_IMAGE_ASPECT_DEPTH_BIT);
    barrier_image(imgs.prev_depth_image,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                  VK_IMAGE_ASPECT_DEPTH_BIT);
    imgs.prev_depth_valid = true;

    // ── 7. Barriers for upsample ──
    barrier_image(imgs.resolved_image,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    barrier_image(imgs.indirect_image,
                  VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);

    // ── 8. Dispatch upsample ──
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, upsample_pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                upsample_pipeline_layout_, 0, 1, &imgs.upsample_set, 0, nullptr);

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

    // ── 9. Final barrier: indirect → shader read ──
    barrier_image(imgs.indirect_image,
                  VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);

    frame_counter_++;
}

} // namespace fjell
