#include "renderer/pass/fog_density_field_pass.hpp"

#include "core/base_path.hpp"
#include "core/profiler.hpp"
#include "environment/environment_state.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/gpu/vma_image.hpp"
#include "renderer/gpu_context.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/pass_registry.hpp"
#include "renderer/resources/shader_utils.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace fjell {

namespace {

void batch_barriers(VkCommandBuffer cmd, const VkImageMemoryBarrier2* barriers, uint32_t count) {
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = count;
    dep.pImageMemoryBarriers = barriers;
    vkCmdPipelineBarrier2(cmd, &dep);
}

VkImageMemoryBarrier2 make_barrier(VkImage image,
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

} // namespace

Result<> FogDensityFieldPass::create(Device& device, const PassResources& resources) {
    device_ = device.handle();
    allocator_ = resources.allocator;
    vk_pipeline_cache_ = resources.pipeline_cache;

    // set 0: the sources to splat, last frame's field, this frame's field.
    std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
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

    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = static_cast<uint32_t>(bindings.size());
    li.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(device_, &li, nullptr, &set_layout_) != VK_SUCCESS) {
        return make_error("FogDensityFieldPass: failed to create descriptor set layout");
    }

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push.size = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &set_layout_;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(device_, &pli, nullptr, &pipeline_layout_) != VK_SUCCESS) {
        return make_error("FogDensityFieldPass: failed to create pipeline layout");
    }

    auto code = read_spirv(base_path::resolve("shaders/fog_density_field.comp.spv"));
    auto* module = create_shader_module(device_, code);
    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module;
    stage.pName = "main";
    VkComputePipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage = stage;
    ci.layout = pipeline_layout_;
    const VkResult r = vkCreateComputePipelines(device_, vk_pipeline_cache_, 1, &ci,
                                                nullptr, &pipeline_);
    vkDestroyShaderModule(device_, module, nullptr);
    if (r != VK_SUCCESS) {
        return make_error("FogDensityFieldPass: failed to create compute pipeline");
    }

    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    // Outside the box the air is undisturbed, which is what a zero border says.
    si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (vkCreateSampler(device_, &si, nullptr, &sampler_) != VK_SUCCESS) {
        return make_error("FogDensityFieldPass: failed to create sampler");
    }

    for (auto& field : fields_) {
        VkImageCreateInfo ii{};
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType = VK_IMAGE_TYPE_3D;
        ii.format = FIELD_FORMAT;
        ii.extent = {RESOLUTION_XZ, RESOLUTION_Y, RESOLUTION_XZ};
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (create_vma_image(allocator_, &ii, &field.image, &field.memory) != VK_SUCCESS) {
            return make_error("FogDensityFieldPass: failed to create field image");
        }
        set_image_debug_name(device_, field.image, "fog_density_field");

        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = field.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_3D;
        vi.format = FIELD_FORMAT;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device_, &vi, nullptr, &field.view) != VK_SUCCESS) {
            return make_error("FogDensityFieldPass: failed to create field view");
        }
    }

    for (auto& buffer : source_buffers_) {
        buffer = std::make_unique<Buffer>(
            allocator_, MAX_SOURCES * sizeof(GpuSource),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
        buffer->map();
    }

    return {};
}

void FogDensityFieldPass::destroy() {
    if (device_ == VK_NULL_HANDLE) return;
    for (auto& buffer : source_buffers_) buffer.reset();
    for (auto& field : fields_) {
        if (field.view != VK_NULL_HANDLE) vkDestroyImageView(device_, field.view, nullptr);
        destroy_vma_image(allocator_, field.image, field.memory);
        field = {};
    }
    if (sampler_ != VK_NULL_HANDLE) vkDestroySampler(device_, sampler_, nullptr);
    if (pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device_, pipeline_, nullptr);
    if (pipeline_layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    }
    if (set_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
    }
    sampler_ = VK_NULL_HANDLE;
    pipeline_ = VK_NULL_HANDLE;
    pipeline_layout_ = VK_NULL_HANDLE;
    set_layout_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
}

void FogDensityFieldPass::post_create(GpuContext& gpu, PassRegistry& /*passes*/) {
    // A field for anything reading before the pass has ever run. Which of the
    // two a reader should see changes every frame, so the binding is rewritten
    // in prepare_frame; this only points the first frame's descriptor at a real
    // image.
    auto& gb = gpu.global_bindings();
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        gb.write_image(i, "fog_density_field", fields_[cur_].view, sampler_,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    gb.flush_writes();
}

void FogDensityFieldPass::prepare_frame(const FrameContext& ctx, GpuContext& gpu) {
    source_count_ = 0;
    moving_count_ = 0;
    if (ctx.environment != nullptr) {
        const auto& sources = ctx.environment->motion_sources;
        source_count_ = std::min(static_cast<uint32_t>(sources.size()), MAX_SOURCES);
        auto* dst = static_cast<GpuSource*>(source_buffers_[ctx.frame_index]->mapped());
        for (uint32_t i = 0; i < source_count_; ++i) {
            const auto& s = sources[i];
            dst[i] = GpuSource{
                .position_radius = glm::vec4(s.position, s.radius),
                .velocity_reach = glm::vec4(s.velocity, s.height),
                .strength = glm::vec4(s.strength, 0.0f, 0.0f, 0.0f),
            };
            if (glm::length(s.velocity) > STILL_SPEED) { ++moving_count_; }
        }
    }

    // The loudest any volume asks the rolls to be. A wake is the body's, not
    // the volume's, so one number serves the frame.
    vortex_strength_ = 0.0f;
    compression_ = 0.0f;
    if (ctx.environment != nullptr) {
        for (const auto& volume : ctx.environment->fog_volumes) {
            vortex_strength_ = std::max(vortex_strength_, volume.vortex_strength);
            compression_ = std::max(compression_, volume.compression);
        }
    }

    // A box around the camera, snapped to whole texels on every axis. What the
    // field holds then stands still on the ground: with nothing moving, the
    // step back that advection takes lands on a texel centre and reads the
    // value stored there rather than a blend of eight of them. Unsnapped, every
    // texel would be resampled every frame, and a field resampled once a frame
    // dissolves while the camera moves and sharpens when it stops.
    const float texel_xz = EXTENT_METRES / static_cast<float>(RESOLUTION_XZ);
    const float texel_y = HEIGHT_METRES / static_cast<float>(RESOLUTION_Y);
    const glm::vec3 eye = ctx.camera_position;
    const float half = EXTENT_METRES * 0.5f;

    // The box follows the camera across the ground but hangs from whatever is
    // stirring the air, not from the eye. A camera looking down from above —
    // an editor view, a third-person shot — would otherwise carry the box up
    // with it and leave the wake at somebody's feet outside the bottom of it.
    // The lowest source in the scene is what has to be covered; the eye only
    // decides which patch of ground is worth keeping.
    float floor_y = eye.y - FLOOR_BELOW_EYE;
    if (source_count_ > 0) {
        const auto* src = static_cast<const GpuSource*>(source_buffers_[ctx.frame_index]->mapped());
        float lowest = src[0].position_radius.y;
        for (uint32_t i = 1; i < source_count_; ++i) {
            lowest = std::min(lowest, src[i].position_radius.y);
        }
        floor_y = std::min(floor_y, lowest - FLOOR_BELOW_SOURCE);
    }

    const glm::vec3 origin{
        std::floor((eye.x - half) / texel_xz) * texel_xz,
        std::floor(floor_y / texel_y) * texel_y,
        std::floor((eye.z - half) / texel_xz) * texel_xz,
    };

    // The box only moves on a frame that carries the field along with it. A
    // frame that records nothing leaves both where they were: moving the box
    // without a dispatch to resample into it would leave the contents
    // describing one patch of world while everything reading them assumed
    // another, and the field would jump the moment the camera crossed a texel
    // while the pass was idle.
    const bool will_step = moving_count_ > 0 || !settled_;
    if (will_step) {
        prev_origin_ = first_frame_ ? origin : origin_;
        origin_ = origin;
    }

    // World position -> box uvw, for the environment UBO. All three axes carry,
    // unlike the motion field's flat map.
    world_to_uv_ = glm::mat4(0.0f);
    world_to_uv_[0][0] = 1.0f / EXTENT_METRES;
    world_to_uv_[1][1] = 1.0f / HEIGHT_METRES;
    world_to_uv_[2][2] = 1.0f / EXTENT_METRES;
    world_to_uv_[3][0] = -origin_.x / EXTENT_METRES;
    world_to_uv_[3][1] = -origin_.y / HEIGHT_METRES;
    world_to_uv_[3][2] = -origin_.z / EXTENT_METRES;
    world_to_uv_[3][3] = 1.0f;

    // Alive while anything moves, and for as long afterwards as a disturbance
    // takes to settle. A scene nobody has walked through costs nothing.
    active_ = moving_count_ > 0 || !settled_;

    // Which field this frame writes, decided here so the binding can be pointed
    // at it before anything records: a descriptor may not be rewritten once a
    // command buffer referencing it is in flight.
    stepped_this_frame_ = false;
    if (active_) { cur_ = prev_index(); }

    auto& gb = gpu.global_bindings();
    gb.write_image(ctx.frame_index, "fog_density_field", fields_[cur_].view, sampler_,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    gb.flush_writes();
}

PassSetup FogDensityFieldPass::setup() const {
    return {
        .name = "fog_density_field",
        .phase = RenderPhase::Environment,
        .sub_order = 71,   // reads the motion field written just before it
    };
}

void FogDensityFieldPass::declare(PassBuilder& builder, const DeclareContext& /*ctx*/) {
    // Pass-private images bound through the global descriptor; consumers reach
    // them outside the catalog.
    builder.has_side_effects();
}

void FogDensityFieldPass::record(const FrameContext& ctx) {
    if (moving_count_ == 0 && settled_) return;
    if (stepped_this_frame_) return;
    stepped_this_frame_ = true;

    FJELL_PROFILE_SCOPE_N("record_fog_density_field");
    VkCommandBuffer cmd = ctx.cmd;

    const uint32_t write = cur_;
    const uint32_t read = 1 - cur_;

    std::array<VkImageMemoryBarrier2, 2> to_use{
        make_barrier(fields_[write].image,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL),
        make_barrier(fields_[read].image,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            first_frame_ ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
    };
    batch_barriers(cmd, to_use.data(), static_cast<uint32_t>(to_use.size()));

    FrameCacheBinding b[3]{};
    b[0].binding = 0;
    b[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[0].buffer.buffer = source_buffers_[ctx.frame_index]->handle();
    b[0].buffer.range = MAX_SOURCES * sizeof(GpuSource);
    b[1].binding = 1;
    b[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[1].image.sampler = sampler_;
    b[1].image.imageView = fields_[read].view;
    b[1].image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b[2].binding = 2;
    b[2].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[2].image.imageView = fields_[write].view;
    b[2].image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorSet set = ctx.descriptor_cache->acquire(set_layout_, {b, 3});

    const float texel_xz = EXTENT_METRES / static_cast<float>(RESOLUTION_XZ);
    const float texel_y = HEIGHT_METRES / static_cast<float>(RESOLUTION_Y);

    // The sustained wind and not the gust: a gust breathes about once a second
    // and stirred air has more mass than that, the same reading fog volumes
    // take of it.
    glm::vec3 wind{0.0f};
    if (ctx.environment != nullptr) {
        const auto& weather = ctx.environment->weather;
        if (glm::length(weather.wind_direction) > 1e-4f) {
            wind = glm::normalize(weather.wind_direction) * weather.wind_speed;
        }
    }

    PushConstants push{};
    push.origin_texel = glm::vec4(origin_, texel_xz);
    push.prev_origin_texel_y = glm::vec4(prev_origin_, texel_y);
    push.extent_decay_dt = glm::vec4(EXTENT_METRES, HEIGHT_METRES,
                                     1.0f / MEMORY_SECONDS, ctx.delta_time);
    push.wind_fraction = glm::vec4(wind, WIND_FRACTION);
    push.res_count = glm::uvec4(RESOLUTION_XZ, RESOLUTION_Y, source_count_, 0);
    push.tuning = glm::vec4(SPREAD_PER_SECOND, WAKE_WIDTH,
                            SWIRL_METRES_PER_SECOND, CARVE_PER_SECOND);
    push.time = elapsed_;
    push.buoyancy = BUOYANCY_METRES_PER_SECOND;
    push.vortex_strength = vortex_strength_;
    push.compression = compression_;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout_,
                            0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);
    const uint32_t groups_xz = (RESOLUTION_XZ + 3) / 4;
    const uint32_t groups_y = (RESOLUTION_Y + 3) / 4;
    vkCmdDispatch(cmd, groups_xz, groups_y, groups_xz);

    auto to_read = make_barrier(fields_[write].image,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    batch_barriers(cmd, &to_read, 1);

    first_frame_ = false;
    elapsed_ += ctx.delta_time;
    // Settled once nothing has moved for as long as a disturbance lasts: by
    // then the last of it has decayed and further frames would write the same
    // empty field again.
    still_seconds_ = moving_count_ > 0 ? 0.0f : still_seconds_ + ctx.delta_time;
    settled_ = still_seconds_ >= MEMORY_SECONDS;
}

} // namespace fjell
