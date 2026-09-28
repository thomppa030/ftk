#include "renderer/pass/motion_field_pass.hpp"
#include "gpu/vulkan/native.hpp"

#include "core/base_path.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "environment/environment_state.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/gpu/shader_utils.hpp"
#include "renderer/gpu/vma_image.hpp"
#include "renderer/gpu_context.hpp"
#include "renderer/pass/pass_gpu_zone.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/pass_registry.hpp"

#include <backends/imgui_impl_vulkan.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace fjell {

Result<> MotionFieldPass::create(Device& device, const PassResources& resources) {
    device_ = device.handle();
    gpu_device_ = resources.device;
    allocator_ = resources.allocator;
    vk_pipeline_cache_ = resources.pipeline_cache;

    // set 0: the sources to splat, last frame's map, this frame's map.
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
        return make_error("MotionFieldPass: failed to create descriptor set layout");
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
        return make_error("MotionFieldPass: failed to create pipeline layout");
    }

    auto code = read_spirv(base_path::resolve("shaders/motion_field.comp.spv"));
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
    if (r != VK_SUCCESS) return make_error("MotionFieldPass: failed to create compute pipeline");

    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    // Outside the map nothing has moved, which is what a zero border says.
    si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (vkCreateSampler(device_, &si, nullptr, &sampler_) != VK_SUCCESS) {
        return make_error("MotionFieldPass: failed to create sampler");
    }

    for (uint32_t i = 0; i < 2; ++i) {
        VkImageCreateInfo ii{};
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = FIELD_FORMAT;
        ii.extent = {RESOLUTION, RESOLUTION, 1};
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                   VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (create_vma_image(allocator_, &ii, &maps_[i].image, &maps_[i].memory) != VK_SUCCESS) {
            return make_error("MotionFieldPass: failed to create map image");
        }
        set_image_debug_name(device_, maps_[i].image, "motion_field");

        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = maps_[i].image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = FIELD_FORMAT;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device_, &vi, nullptr, &maps_[i].view) != VK_SUCCESS) {
            return make_error("MotionFieldPass: failed to create map view");
        }
        maps_[i].texture = gpu::vulkan::adopt(*gpu_device_, maps_[i].image, maps_[i].view, ii);
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

void MotionFieldPass::destroy() {
    if (device_ == VK_NULL_HANDLE) return;
    for (auto& buffer : source_buffers_) buffer.reset();
    for (auto& map : maps_) {
        if (map.view != VK_NULL_HANDLE) vkDestroyImageView(device_, map.view, nullptr);
        destroy_vma_image(allocator_, map.image, map.memory);
        map = {};
    }
    if (sampler_ != VK_NULL_HANDLE) vkDestroySampler(device_, sampler_, nullptr);
    if (pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device_, pipeline_, nullptr);
    if (pipeline_layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    if (set_layout_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
    sampler_ = VK_NULL_HANDLE;
    pipeline_ = VK_NULL_HANDLE;
    pipeline_layout_ = VK_NULL_HANDLE;
    set_layout_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
}

void MotionFieldPass::post_create(GpuContext& gpu, PassRegistry& /*passes*/) {
    // A first map for anything that reads before the pass has ever run. Which
    // of the two a reader should see changes every frame, so the binding is
    // rewritten in prepare_frame(); this only makes the very first frame's
    // descriptor point at a real image rather than nothing.
    auto& gb = gpu.global_bindings();
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        gb.write_image(i, "motion_field", maps_[cur_].view, sampler_,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    gb.flush_writes();
}

void MotionFieldPass::prepare_frame(const FrameContext& ctx, GpuContext& gpu) {
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
                // World Y of the top of what moved, not its height above
                // itself: a consumer asking "is this froxel above what came
                // through" needs somewhere to measure from.
                .velocity_height = glm::vec4(s.velocity, s.position.y + s.height),
                .strength = glm::vec4(s.strength, 0.0f, 0.0f, 0.0f),
            };
            // A source standing still keeps its own ground marked but stirs
            // nothing new, which is the difference between the map needing
            // another frame of work and having settled.
            if (glm::length(s.velocity) > STILL_SPEED) ++moving_count_;
        }
    }

    // A square around the camera, snapped to whole texels so what the map
    // holds stands still on the ground as the camera moves. Re-centring every
    // frame would resample the whole map and smear it away.
    const float texel = EXTENT_METRES / static_cast<float>(RESOLUTION);
    const glm::vec3 eye = ctx.camera_position;
    const float half = EXTENT_METRES * 0.5f;
    const glm::vec2 origin{std::floor((eye.x - half) / texel) * texel,
                           std::floor((eye.z - half) / texel) * texel};
    floor_y_ = eye.y - half;

    prev_origin_ = first_frame_ ? origin : origin_;
    origin_ = origin;

    // World XZ → map uv, for the environment UBO. Consumers use it rather
    // than rebuilding the projection and drifting from it.
    // World XZ -> map uv. Zero, not identity: an identity's [1][1] would
    // carry world Y into v and send every froxel above a metre outside the
    // map, where the lookup reads as nothing having moved.
    world_to_uv_ = glm::mat4(0.0f);
    world_to_uv_[0][0] = 1.0f / EXTENT_METRES;   // x -> u
    world_to_uv_[2][1] = 1.0f / EXTENT_METRES;   // z -> v
    world_to_uv_[3][0] = -origin.x / EXTENT_METRES;
    world_to_uv_[3][1] = -origin.y / EXTENT_METRES;
    world_to_uv_[3][3] = 1.0f;

    // Alive while anything moves, and for as long afterwards as the map takes
    // to fade. A scene where everyone has stood still for a while costs
    // nothing: the marks under their feet have faded and nothing replaces them.
    active_ = moving_count_ > 0 || !settled_;

    // Which of the two maps this frame writes is decided here rather than
    // while recording, because the map a reader sees is bound now and a
    // descriptor may not be rewritten once a command buffer referencing it is
    // in flight. The pass alternates, so without this the binding would keep
    // pointing at whichever map it was given at startup: right on the frames
    // that happen to land on it, and a read of the map being written on the
    // rest.
    if (active_) cur_ = prev_index();

    // Readers are pointed at the map this frame writes, not the last one.
    // Everything that reads it runs later in the same command buffer, behind
    // the barrier record() ends with, so what they sample is this frame's
    // finished map rather than a frame-old one.
    auto& gb = gpu.global_bindings();
    gb.write_image(ctx.frame_index, "motion_field", maps_[cur_].view, sampler_,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    gb.flush_writes();
}

PassSetup MotionFieldPass::setup() const {
    return {
        .name = "motion_field",
        .phase = RenderPhase::Environment,
        .sub_order = 70,   // after the cloud shadow, before anything shades
    };
}

// Where the map is read: the surfaces and the editor's debug view in the
// fragment shader, the fog in compute. A reader at any other stage declares
// its read and gets its own barrier from the graph.
constexpr gpu::AccessSet READERS = gpu::Access::sampled_fragment | gpu::Access::sampled_compute;

void MotionFieldPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    // Nothing moves and the marks have faded: the map is left as it is.
    if (moving_count_ == 0 && settled_) { return; }
    // Last frame's map carried forward into this frame's. The graph moves
    // the one written into GENERAL, and back to rest afterwards.
    builder.read(builder.import_named(ctx, "motion_field_prev"),
                 gpu::Access::sampled_compute);
    builder.write(builder.import_named(ctx, "motion_field"), gpu::Access::storage_write_compute);
}

void MotionFieldPass::collect_exports(DeclareContext& ctx) {
    if (maps_[cur_].image == VK_NULL_HANDLE) { return; }
    // Both maps rest where their readers sample them; neither holds anything
    // before the pass first runs.
    auto add = [&](std::string_view name, const Map& map) {
        ImportedImage img{};
        img.view = map.texture;
        img.persistent = true;   // carried into the next frame's map
        img.resting = READERS;
        img.unwritten = first_frame_;
        ctx.imports.emplace(fg_name_hash(name), img);
    };
    // The map this frame writes is the one the global set binds.
    add("motion_field", maps_[cur_]);
    add("motion_field_prev", maps_[prev_index()]);
}

void MotionFieldPass::record(const FrameContext& ctx) {
    FJELL_PROFILE_SCOPE_N("record_motion_field");
    VkCommandBuffer cmd = ctx.cmd;

    if (moving_count_ == 0 && settled_) return;
    FJELL_GPU_ZONE(ctx, "Motion Field");

    // prepare_frame chose which map this frame writes, so that readers could
    // be pointed at the other one before any of this was recorded.
    const uint32_t write = cur_;
    const uint32_t read = 1 - cur_;

    const float texel = EXTENT_METRES / static_cast<float>(RESOLUTION);
    // How far the map slid since last frame, in texels: where this frame's
    // texel sits, expressed in the previous frame's grid. Both origins are
    // snapped, so this is a whole number and the read lands on a texel centre
    // rather than between two.
    const glm::vec2 scroll = (origin_ - prev_origin_) / texel;

    // The graph has the map written in GENERAL and the one read in
    // SHADER_READ_ONLY_OPTIMAL.
    FrameCacheBinding b[3]{};
    b[0].binding = 0;
    b[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[0].buffer.buffer = source_buffers_[ctx.frame_index]->handle();
    b[0].buffer.range = MAX_SOURCES * sizeof(GpuSource);
    b[1].binding = 1;
    b[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[1].image.sampler = sampler_;
    b[1].image.imageView = maps_[read].view;
    b[1].image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b[2].binding = 2;
    b[2].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[2].image.imageView = maps_[write].view;
    b[2].image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorSet set = ctx.descriptor_cache->acquire(set_layout_, std::span{b, 3});

    PushConstants push{};
    push.map_origin_extent = glm::vec4(origin_.x, origin_.y, EXTENT_METRES, texel);
    push.scroll_decay = glm::vec4(scroll.x, scroll.y, 1.0f / MEMORY_SECONDS, ctx.delta_time);
    push.source_count = source_count_;
    push.resolution = RESOLUTION;
    push.spread = SPREAD_PER_SECOND;
    push.wake_width = WAKE_WIDTH;
    push.swirl = SWIRL_METRES_PER_SECOND;
    push.time = elapsed_;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout_,
                            0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);
    const uint32_t groups = (RESOLUTION + 7) / 8;
    vkCmdDispatch(cmd, groups, groups, 1);
    first_frame_ = false;
    // Settled once nothing has moved for as long as the map remembers: by
    // then the last mark has decayed to nothing and further frames would
    // write the same empty map again.
    elapsed_ += ctx.delta_time;
    still_seconds_ = moving_count_ > 0 ? 0.0f : still_seconds_ + ctx.delta_time;
    settled_ = still_seconds_ >= MEMORY_SECONDS;
}

VkDescriptorSet MotionFieldPass::imgui_texture(VkSampler sampler) {
    if (imgui_set_ == VK_NULL_HANDLE && maps_[cur_].view != VK_NULL_HANDLE) {
        imgui_set_ = ImGui_ImplVulkan_AddTexture(sampler, maps_[cur_].view,
                                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return imgui_set_;
}

} // namespace fjell
