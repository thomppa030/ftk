#include "renderer/pass/rt_reflection_pass.hpp"

#include "core/base_path.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "renderer/gpu/accel_structure.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass/ddgi_pass.hpp"
#include "renderer/pass/pass_gpu_zone.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/post_process_resources.hpp"
#include "renderer/resource_registry.hpp"
#include "renderer/resources/mega_buffer.hpp"
#include "renderer/resources/shader_utils.hpp"

#include <array>
#include <cstring>

namespace fjell {

namespace {

// Push constant for the rgen + rchit. Kept small (16 bytes) so we have
// budget elsewhere if needed.
struct ReflectionPush {
    uint32_t frame_index;
    float max_trace_dist;
    float intensity;
    float pad;
};

// UBO at binding 14: sun lighting only. Everything else needed by the
// hit shader (camera, projection inverse, DDGI grid info) lives in the
// DDGI UBO at binding 0, which we share with DdgiPass.
struct ReflectionUbo {
    glm::vec4 sun_dir_intensity;
    glm::vec4 sun_color;
};

// Round up to multiple of `alignment`.
constexpr uint32_t align_up(uint32_t v, uint32_t alignment) {
    return (v + alignment - 1) & ~(alignment - 1);
}

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

    // RT pipeline only built when device supports it. Without RT the pass
    // still exists (so .fjgraph parses) but record() returns immediately.
    if (!device.ray_tracing_supported()) {
        FJELL_GFX_INFO("RT reflections disabled (no VK_KHR_ray_tracing_pipeline)");
        return {};
    }

    // ── Trace descriptor set layout (15 bindings) ──
    constexpr uint32_t BINDING_COUNT = 15;
    constexpr VkShaderStageFlags ALL_RT =
        VK_SHADER_STAGE_RAYGEN_BIT_KHR
        | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR
        | VK_SHADER_STAGE_MISS_BIT_KHR;

    std::array<VkDescriptorSetLayoutBinding, BINDING_COUNT> bindings{};
    bindings[0]  = {0,  VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,            1, ALL_RT, nullptr};
    bindings[1]  = {1,  VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,1, VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr};
    bindings[2]  = {2,  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,    1, VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr};
    bindings[3]  = {3,  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,    1, VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr};
    bindings[4]  = {4,  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,    1, VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr};
    bindings[5]  = {5,  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,    1, VK_SHADER_STAGE_MISS_BIT_KHR,    nullptr};
    bindings[6]  = {6,  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,             1, VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr};
    bindings[7]  = {7,  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,            1, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, nullptr};
    bindings[8]  = {8,  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,            1, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, nullptr};
    bindings[9]  = {9,  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,            1, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, nullptr};
    bindings[10] = {10, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,    1, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, nullptr};
    bindings[11] = {11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,    1, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, nullptr};
    bindings[12] = {12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,    1, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, nullptr};
    bindings[13] = {13, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,            1, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, nullptr};
    bindings[14] = {14, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,            1, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, nullptr};

    // UPDATE_AFTER_BIND on per-frame inputs (TLAS, screen-space samplers,
    // DDGI atlases, instance data).
    std::array<VkDescriptorBindingFlags, BINDING_COUNT> flags{};
    flags[1]  = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; // TLAS
    flags[2]  = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; // depth
    flags[3]  = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; // normal
    flags[4]  = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; // mr
    flags[5]  = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; // sky
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
    push.stageFlags = ALL_RT;
    push.size = sizeof(ReflectionPush);

    std::array<VkDescriptorSetLayout, 2> layouts = {trace_set_layout_, resources.material_set_layout};
    VkPipelineLayoutCreateInfo pl_ci{};
    pl_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl_ci.setLayoutCount = static_cast<uint32_t>(layouts.size());
    pl_ci.pSetLayouts = layouts.data();
    pl_ci.pushConstantRangeCount = 1;
    pl_ci.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(vk_device_, &pl_ci, nullptr, &rt_pipeline_layout_) != VK_SUCCESS) {
        return make_error("Failed to create RT reflection pipeline layout");
    }

    // ── RT pipeline ──
    auto rgen_spv  = read_spirv(base_path::resolve("shaders/rt_reflection.rgen.spv"));
    auto rchit_spv = read_spirv(base_path::resolve("shaders/rt_reflection.rchit.spv"));
    auto rmiss_spv = read_spirv(base_path::resolve("shaders/rt_reflection.rmiss.spv"));

    auto* rgen_mod  = create_shader_module(vk_device_, rgen_spv);
    auto* rchit_mod = create_shader_module(vk_device_, rchit_spv);
    auto* rmiss_mod = create_shader_module(vk_device_, rmiss_spv);

    std::array<VkPipelineShaderStageCreateInfo, 3> stages{};
    for (auto& s : stages) s.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;     stages[0].module = rgen_mod;  stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_MISS_BIT_KHR;       stages[1].module = rmiss_mod; stages[1].pName = "main";
    stages[2].stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;stages[2].module = rchit_mod; stages[2].pName = "main";

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
    rt_ci.layout = rt_pipeline_layout_;

    VkResult rt_result = device.create_rt_pipelines_fn()(
        vk_device_, VK_NULL_HANDLE, resources.pipeline_cache,
        1, &rt_ci, nullptr, &rt_pipeline_);

    vkDestroyShaderModule(vk_device_, rgen_mod, nullptr);
    vkDestroyShaderModule(vk_device_, rchit_mod, nullptr);
    vkDestroyShaderModule(vk_device_, rmiss_mod, nullptr);

    if (rt_result != VK_SUCCESS) {
        return make_error("Failed to create RT reflection pipeline");
    }

    // ── Shader binding table ──
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rt_props{};
    rt_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
    VkPhysicalDeviceProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &rt_props;
    vkGetPhysicalDeviceProperties2(device.physical_device(), &props2);

    const uint32_t handle_size = rt_props.shaderGroupHandleSize;
    const uint32_t handle_alignment = rt_props.shaderGroupHandleAlignment;
    const uint32_t base_alignment = rt_props.shaderGroupBaseAlignment;
    const uint32_t handle_aligned = align_up(handle_size, handle_alignment);
    const uint32_t group_size = align_up(handle_aligned, base_alignment);

    constexpr uint32_t GROUP_COUNT = 3;
    std::vector<uint8_t> handles(static_cast<size_t>(GROUP_COUNT) * handle_size);
    device.get_rt_shader_group_handles_fn()(vk_device_, rt_pipeline_, 0,
                                              GROUP_COUNT, handles.size(), handles.data());

    const VkDeviceSize sbt_total = static_cast<VkDeviceSize>(group_size) * GROUP_COUNT;
    sbt_buffer_ = std::make_unique<Buffer>(
        resources.allocator, sbt_total,
        VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR
        | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
        | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);

    auto* sbt_data = static_cast<uint8_t*>(sbt_buffer_->mapped());
    if (sbt_data == nullptr) {
        sbt_buffer_->map();
        sbt_data = static_cast<uint8_t*>(sbt_buffer_->mapped());
    }
    std::memcpy(sbt_data,                        handles.data(),                     handle_size);
    std::memcpy(sbt_data + group_size,           handles.data() + handle_size,       handle_size);
    std::memcpy(sbt_data + group_size * 2,       handles.data() + handle_size * 2,   handle_size);

    VkBufferDeviceAddressInfo addr_info{};
    addr_info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    addr_info.buffer = sbt_buffer_->handle();
    VkDeviceAddress sbt_addr = device.get_buffer_device_address_fn()(vk_device_, &addr_info);

    sbt_raygen_   = {sbt_addr,                    group_size, group_size};
    sbt_miss_     = {sbt_addr + group_size,       group_size, group_size};
    sbt_hit_      = {sbt_addr + group_size * 2,   group_size, group_size};
    sbt_callable_ = {};

    // ── Reflection UBO + descriptor pool + set ──
    state_.reflection_ubo = std::make_unique<Buffer>(
        resources.allocator, sizeof(ReflectionUbo),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    state_.reflection_ubo->map();

    std::array<VkDescriptorPoolSize, 5> pool_sizes{};
    pool_sizes[0] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,             2}; // DDGI UBO + reflection UBO
    pool_sizes[1] = {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1};
    pool_sizes[2] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,     7}; // depth, normal, mr, sky, irr, vis, probe_off
    pool_sizes[3] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,              1}; // reflection_out
    pool_sizes[4] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             4}; // vertex, index, instance, probe_status

    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pool_ci.maxSets = 1;
    pool_ci.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
    pool_ci.pPoolSizes = pool_sizes.data();
    if (vkCreateDescriptorPool(vk_device_, &pool_ci, nullptr, &state_.desc_pool) != VK_SUCCESS) {
        return make_error("Failed to create RT reflection descriptor pool");
    }

    VkDescriptorSetAllocateInfo set_alloc{};
    set_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    set_alloc.descriptorPool = state_.desc_pool;
    set_alloc.descriptorSetCount = 1;
    set_alloc.pSetLayouts = &trace_set_layout_;
    if (vkAllocateDescriptorSets(vk_device_, &set_alloc, &state_.trace_set) != VK_SUCCESS) {
        return make_error("Failed to allocate RT reflection descriptor set");
    }

    FJELL_GFX_INFO("RT reflection pass created");
    return {};
}

void RtReflectionPass::destroy() {
    if (vk_device_ == VK_NULL_HANDLE) return;

    state_.reflection_ubo.reset();
    if (state_.desc_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(vk_device_, state_.desc_pool, nullptr);
        state_.desc_pool = VK_NULL_HANDLE;
        state_.trace_set = VK_NULL_HANDLE;
    }
    state_.first_use = true;

    sbt_buffer_.reset();

    if (rt_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk_device_, rt_pipeline_, nullptr);
        rt_pipeline_ = VK_NULL_HANDLE;
    }
    if (rt_pipeline_layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(vk_device_, rt_pipeline_layout_, nullptr);
        rt_pipeline_layout_ = VK_NULL_HANDLE;
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
    // Always declare the resource so the composite can import it
    // unconditionally. When not active we clear it to black.
    auto depth = builder.import_named(ctx, "depth");
    auto normal = builder.import_named(ctx, "normal");
    auto mr = builder.import_named(ctx, "mr");

    TextureDesc out_desc{};
    out_desc.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    out_desc.size_class = SizeClass::viewport;
    auto rt_out = builder.create("rt_reflection_output", out_desc);

    if (ray_traced_active()) {
        builder.read(depth, ResourceAccess::sampled_compute);
        builder.read(normal, ResourceAccess::sampled_compute);
        builder.read(mr, ResourceAccess::sampled_compute);
        builder.write(rt_out, ResourceAccess::storage_write_compute);
    } else {
        // Clear path: graph hands us TRANSFER_DST, we clear, transition to
        // SHADER_READ for the composite.
        builder.write(rt_out, ResourceAccess::transfer_dst);
        builder.final_layout(rt_out, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                             VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
}

void RtReflectionPass::record(const FrameContext& ctx) {
    if (ctx.registry == nullptr) return;
    auto* rt_res = ctx.registry->find_image("rt_reflection_output");
    if (rt_res == nullptr || rt_res->image == VK_NULL_HANDLE) return;

    FJELL_PROFILE_SCOPE_N("record_rt_reflection");

    // ── Inactive path: clear and transition ──
    if (!ray_traced_active()) {
        VkClearColorValue clear{};
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(ctx.cmd, rt_res->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &clear, 1, &range);
        emit_image_barrier(ctx.cmd, rt_res->image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        return;
    }

    // ── Active path: prerequisites ──
    if (accel_manager_ == nullptr || !accel_manager_->tlas_valid(ctx.frame_index)) return;
    if (ctx.target == nullptr) return;
    if (ctx.pp_resources == nullptr) return;
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
        std::memcpy(state_.reflection_ubo->mapped(), &u, sizeof(u));
    }

    auto cmd = ctx.cmd;
    auto extent = ctx.target->extent();

    // ── Pre-trace layout transitions ──
    emit_image_barrier(cmd, rt_res->image,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
        VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_SHADER_WRITE_BIT);

    // Extend depth/normal/MR visibility to the RT stage. Same layout on
    // both sides — execution dependency only.
    emit_image_barrier(cmd, ctx.target->depth_image(),
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT
            | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT
            | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
        VK_ACCESS_2_SHADER_READ_BIT,
        VK_IMAGE_ASPECT_DEPTH_BIT);
    emit_image_barrier(cmd, ctx.target->normal_image(),
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
        VK_ACCESS_2_SHADER_READ_BIT);
    emit_image_barrier(cmd, ctx.target->mr_image(),
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
        VK_ACCESS_2_SHADER_READ_BIT);

    // ── Update descriptor set ──
    // First-use writes: static bindings (vertex/index SSBOs, output, UBOs).
    if (state_.first_use) {
        std::array<VkWriteDescriptorSet, 3> static_writes{};
        for (auto& w : static_writes) {
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = state_.trace_set;
            w.descriptorCount = 1;
        }

        VkDescriptorBufferInfo refl_ubo_info{state_.reflection_ubo->handle(), 0, sizeof(ReflectionUbo)};
        static_writes[0].dstBinding = 14;
        static_writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        static_writes[0].pBufferInfo = &refl_ubo_info;

        VkDescriptorBufferInfo vert_info{ctx.mega_buffer->vertex_buffer_handle(), 0, VK_WHOLE_SIZE};
        static_writes[1].dstBinding = 7;
        static_writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        static_writes[1].pBufferInfo = &vert_info;

        VkDescriptorBufferInfo idx_info{ctx.mega_buffer->index_buffer_handle(), 0, VK_WHOLE_SIZE};
        static_writes[2].dstBinding = 8;
        static_writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        static_writes[2].pBufferInfo = &idx_info;

        vkUpdateDescriptorSets(vk_device_, static_cast<uint32_t>(static_writes.size()),
                               static_writes.data(), 0, nullptr);
        state_.first_use = false;
    }

    // Per-frame writes: TLAS + screen images + DDGI atlases + storage output.
    {
        // Reflection storage output.
        VkDescriptorImageInfo out_info{};
        out_info.imageView = rt_res->view;
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
        // matches the rchit's struct without us replicating the data.
        VkDescriptorBufferInfo ddgi_ubo_info{ddgi_imgs->constants_ubo->handle(), 0, VK_WHOLE_SIZE};

        // Probe status SSBO.
        VkDescriptorBufferInfo probe_status_info{ddgi_imgs->probe_status_buffer->handle(), 0, VK_WHOLE_SIZE};

        // RT instance data SSBO.
        VkDescriptorBufferInfo inst_info{accel_manager_->instance_data_buffer(ctx.frame_index), 0, VK_WHOLE_SIZE};

        // TLAS.
        VkAccelerationStructureKHR tlas = accel_manager_->tlas(ctx.frame_index);
        VkWriteDescriptorSetAccelerationStructureKHR tlas_as{};
        tlas_as.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
        tlas_as.accelerationStructureCount = 1;
        tlas_as.pAccelerationStructures = &tlas;

        std::array<VkWriteDescriptorSet, 12> w{};
        for (auto& wi : w) {
            wi.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wi.dstSet = state_.trace_set;
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
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rt_pipeline_);
    std::array<VkDescriptorSet, 2> sets = {state_.trace_set, ctx.bindless_set};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                            rt_pipeline_layout_, 0,
                            static_cast<uint32_t>(sets.size()), sets.data(),
                            0, nullptr);

    ReflectionPush push{};
    push.frame_index = frame_counter_++;
    push.max_trace_dist = 100.0F;
    push.intensity = intensity_;
    vkCmdPushConstants(cmd, rt_pipeline_layout_,
                       VK_SHADER_STAGE_RAYGEN_BIT_KHR
                       | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR
                       | VK_SHADER_STAGE_MISS_BIT_KHR,
                       0, sizeof(push), &push);

    device_->cmd_trace_rays_fn()(cmd,
        &sbt_raygen_, &sbt_miss_, &sbt_hit_, &sbt_callable_,
        extent.width, extent.height, 1);

    // ── Post-trace barrier: GENERAL → SHADER_READ for the composite ──
    emit_image_barrier(cmd, rt_res->image,
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

} // namespace fjell
