#include "renderer/pass/overhead_depth_pass.hpp"

#include "core/base_path.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "environment/environment_state.hpp"
#include "renderer/global_bindings.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/gpu/vk_check.hpp"
#include "renderer/gpu/vma_image.hpp"
#include "renderer/gpu_context.hpp"
#include "renderer/indirect_batch.hpp"
#include "renderer/pass/depth_pipeline.hpp"
#include "renderer/pass/pass_gpu_zone.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/renderer_constants.hpp"
#include "renderer/resources/mega_buffer.hpp"
#include "renderer/resources/shader_utils.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>
#include <stdexcept>

namespace fjell {

Result<> OverheadDepthPass::create(Device& device, const PassResources& resources) {
    device_ = device.handle();
    allocator_ = resources.allocator;
    vk_pipeline_cache_ = resources.pipeline_cache;

    // Sampled as a plain depth texture rather than through a comparison
    // sampler: the readers want the occluder's height, not a shadow test.
    VkSamplerCreateInfo sampler_ci{};
    sampler_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_ci.magFilter = VK_FILTER_NEAREST;
    sampler_ci.minFilter = VK_FILTER_NEAREST;
    sampler_ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampler_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampler_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    // Outside the map nothing shelters: the far plane, so a lookup there
    // reads as open sky.
    sampler_ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    if (vkCreateSampler(device_, &sampler_ci, nullptr, &sampler_) != VK_SUCCESS) {
        return make_error("Failed to create overhead depth sampler");
    }

    // Same shape as a shadow cascade: the light-space matrix through a push
    // constant, the scene's draw data in set 2.
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(glm::mat4);

    std::array set_layouts = {resources.global_set_layout, resources.material_set_layout,
                              resources.draw_data_layout};
    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = static_cast<uint32_t>(set_layouts.size());
    layout_info.pSetLayouts = set_layouts.data();
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_range;
    if (vkCreatePipelineLayout(device_, &layout_info, nullptr, &layout_) != VK_SUCCESS) {
        return make_error("Failed to create overhead depth pipeline layout");
    }

    DepthPipelineConfig config{};
    config.layout = layout_;
    config.depth_format = DEPTH_FORMAT;
    config.samples = VK_SAMPLE_COUNT_1_BIT;
    // Back faces, unlike a shadow cascade: a roof's underside is what
    // shelters, and front-face culling would lose a single-sided one.
    config.cull_mode = VK_CULL_MODE_NONE;
    config.depth_compare = VK_COMPARE_OP_LESS_OR_EQUAL;
    config.depth_clamp = true;
    config.pipeline_cache = vk_pipeline_cache_;
    pipeline_ = create_depth_vertex_pipeline(device_, base_path::resolve("shaders/shadow.vert.spv"), config);
    if (pipeline_ == VK_NULL_HANDLE) {
        return make_error("Failed to create overhead depth pipeline");
    }

    create_image();
    FJELL_GFX_DEBUG("Overhead depth pass created");
    return {};
}

void OverheadDepthPass::post_create(GpuContext& gpu, PassRegistry& /*passes*/) {
    graphics_queue_ = gpu.core().graphics_queue();
    command_pool_ = gpu.core().command_pool();
    seed_layout();

    auto& gb = gpu.global_bindings();
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        gb.write_image(i, "overhead_depth", view_, sampler_,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    gb.flush_writes();
}

void OverheadDepthPass::destroy() {
    if (device_ == VK_NULL_HANDLE) return;
    destroy_image();
    if (pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device_, pipeline_, nullptr);
    if (layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, layout_, nullptr);
    if (sampler_ != VK_NULL_HANDLE) vkDestroySampler(device_, sampler_, nullptr);
    pipeline_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
}

void OverheadDepthPass::create_image() {
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = DEPTH_FORMAT;
    ci.extent = {RESOLUTION, RESOLUTION, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (create_vma_image(allocator_, &ci, &image_, &memory_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create overhead depth image");
    }
    set_image_debug_name(device_, image_, "overhead_depth");

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = image_;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = DEPTH_FORMAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device_, &vi, nullptr, &view_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create overhead depth view");
    }
}

void OverheadDepthPass::destroy_image() {
    if (view_ != VK_NULL_HANDLE) vkDestroyImageView(device_, view_, nullptr);
    view_ = VK_NULL_HANDLE;
    destroy_vma_image(allocator_, image_, memory_);
    image_ = VK_NULL_HANDLE;
    memory_ = VK_NULL_HANDLE;
}

void OverheadDepthPass::seed_layout() {
    if (graphics_queue_ == VK_NULL_HANDLE || image_ == VK_NULL_HANDLE) return;
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = command_pool_;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd{};
    vk_check(vkAllocateCommandBuffers(device_, &alloc, &cmd), "Failed to allocate overhead seed buffer");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(cmd, &begin), "Failed to begin overhead seed buffer");

    // An empty render pass clears it to the far plane, which reads as open
    // sky, and leaves it sampleable before anything looks.
    VkImageMemoryBarrier2 to_attachment{};
    to_attachment.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    to_attachment.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    to_attachment.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
    to_attachment.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    to_attachment.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_attachment.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    to_attachment.image = image_;
    to_attachment.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &to_attachment;
    vkCmdPipelineBarrier2(cmd, &dep);

    VkRenderingAttachmentInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth.imageView = view_;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue.depthStencil = {1.0f, 0};
    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.extent = {RESOLUTION, RESOLUTION};
    ri.layerCount = 1;
    ri.pDepthAttachment = &depth;
    vkCmdBeginRendering(cmd, &ri);
    vkCmdEndRendering(cmd);

    VkImageMemoryBarrier2 to_sampled = to_attachment;
    to_sampled.srcStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    to_sampled.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    to_sampled.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    to_sampled.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    to_sampled.oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    to_sampled.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dep.pImageMemoryBarriers = &to_sampled;
    vkCmdPipelineBarrier2(cmd, &dep);

    vk_check(vkEndCommandBuffer(cmd), "Failed to end overhead seed buffer");
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vk_check(vkQueueSubmit(graphics_queue_, 1, &submit, VK_NULL_HANDLE), "Failed to submit overhead seed");
    vk_check(vkQueueWaitIdle(graphics_queue_), "Failed to wait for overhead seed");
    vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
    cleared_ = true;
}

void OverheadDepthPass::prepare_frame(const FrameContext& ctx, GpuContext& /*gpu*/) {
    active_ = false;
    if (ctx.environment == nullptr) return;
    // Only while something falls or the world is still drying: a clear scene
    // does not pay for a render nothing reads.
    active_ = needed_by(*ctx.environment);

    // A square around the camera, snapped to the map's texels so the shelter
    // stands still on the ground as the camera moves.
    const float texel = EXTENT_METRES / static_cast<float>(RESOLUTION);
    const glm::vec3 eye = ctx.camera_position;
    const float cx = std::floor(eye.x / texel) * texel;
    const float cz = std::floor(eye.z / texel) * texel;
    const float half = EXTENT_METRES * 0.5f;

    // Looking straight down. Depth runs from the top of the slab to its
    // bottom, so a sample's depth is how far below the top an occluder is.
    const glm::vec3 centre{cx, eye.y + HEIGHT_ABOVE, cz};
    const glm::mat4 view = glm::lookAt(centre, centre - glm::vec3(0.0f, 1.0f, 0.0f),
                                       glm::vec3(0.0f, 0.0f, -1.0f));
    glm::mat4 proj = glm::ortho(-half, half, -half, half, 0.0f, HEIGHT_ABOVE + DEPTH_BELOW);
    proj[1][1] *= -1.0f;   // Vulkan's clip space
    world_to_map_ = proj * view;
}

bool OverheadDepthPass::needed_by(const EnvironmentState& environment) {
    const auto& weather = environment.weather;
    return weather.precipitation > 0.0f || weather.wetness > 0.0f || weather.snow > 0.0f;
}

AABB OverheadDepthPass::reach_bounds(const glm::vec3& camera_position) const {
    const float half = EXTENT_METRES * 0.5f;
    return AABB{
        .min = glm::vec3(camera_position.x - half, camera_position.y - DEPTH_BELOW,
                         camera_position.z - half),
        .max = glm::vec3(camera_position.x + half, camera_position.y + HEIGHT_ABOVE,
                         camera_position.z + half),
    };
}

PassSetup OverheadDepthPass::setup() const {
    return {
        .name = "overhead_depth",
        .phase = RenderPhase::Environment,
        .sub_order = 70,   // with the other environment maps, before anything shades
    };
}

void OverheadDepthPass::declare(PassBuilder& builder, const DeclareContext& /*ctx*/) {
    // A pass-private image reached through the global descriptor.
    builder.has_side_effects();
}

void OverheadDepthPass::record(const FrameContext& ctx) {
    if (!active_) {
        // Nothing falling: leave the map as it is. It reads as open sky once
        // cleared, which is the right answer when nothing consults it.
        return;
    }
    if (ctx.overhead_batch == nullptr || ctx.mega_buffer == nullptr) return;
    if (ctx.overhead_draw_data == VK_NULL_HANDLE) return;
    if (ctx.overhead_batch->opaque_draw_count(ctx.frame_index) == 0) return;

    FJELL_PROFILE_SCOPE_N("record_overhead_depth");
    FJELL_GPU_ZONE(ctx, "Overhead Depth");

    VkImageMemoryBarrier2 to_attachment{};
    to_attachment.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    to_attachment.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    to_attachment.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
    to_attachment.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    to_attachment.oldLayout = cleared_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                       : VK_IMAGE_LAYOUT_UNDEFINED;
    to_attachment.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    to_attachment.image = image_;
    to_attachment.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &to_attachment;
    vkCmdPipelineBarrier2(ctx.cmd, &dep);

    VkRenderingAttachmentInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth.imageView = view_;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.extent = {RESOLUTION, RESOLUTION};
    ri.layerCount = 1;
    ri.pDepthAttachment = &depth;
    vkCmdBeginRendering(ctx.cmd, &ri);

    VkViewport viewport{};
    viewport.width = static_cast<float>(RESOLUTION);
    viewport.height = static_cast<float>(RESOLUTION);
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{};
    scissor.extent = {RESOLUTION, RESOLUTION};

    vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdSetViewport(ctx.cmd, 0, 1, &viewport);
    vkCmdSetScissor(ctx.cmd, 0, 1, &scissor);

    // The overhead batch's own draw data: culled against this box, not the
    // view, so shelter off screen is still drawn.
    std::array sets = {ctx.global_descriptor_set, ctx.bindless_set, ctx.overhead_draw_data};
    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0,
                            static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
    vkCmdPushConstants(ctx.cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4),
                       &world_to_map_);

    ctx.mega_buffer->bind(ctx.cmd);
    ctx.overhead_batch->draw(ctx.cmd, ctx.frame_index);

    vkCmdEndRendering(ctx.cmd);

    VkImageMemoryBarrier2 to_sampled = to_attachment;
    to_sampled.srcStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    to_sampled.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    to_sampled.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    to_sampled.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    to_sampled.oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    to_sampled.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dep.pImageMemoryBarriers = &to_sampled;
    vkCmdPipelineBarrier2(ctx.cmd, &dep);
    cleared_ = true;
}

} // namespace fjell
