#include "renderer/pass/sky_cubemap_pass.hpp"
#include "renderer/pass/pass_gpu_zone.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/gpu/vma_image.hpp"
#include "renderer/gpu_context.hpp"
#include "renderer/resource_registry.hpp"
#include "renderer/pass/sky_pass.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/pass_registry.hpp"
#include "renderer/renderer_constants.hpp"
#include "renderer/resources/shader_utils.hpp"
#include "renderer/gpu/vk_check.hpp"
#include "scene/light_node.hpp"
#include "core/project.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#include <stb_image.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace fjell {

Result<> SkyCubemapPass::create(Device& device, const PassResources& resources) {
    device_ = device.handle();
    vk_pipeline_cache_ = resources.pipeline_cache;
    allocator_ = resources.allocator;
    command_pool_ = resources.command_pool;
    queue_ = resources.graphics_queue;

    create_cubemap();
    create_cubemap_pipeline();
    create_brdf_lut();
    create_brdf_pipeline();
    generate_brdf_lut();

    FJELL_GFX_DEBUG("Sky cubemap pass created");
    return {};
}

void SkyCubemapPass::create(VkDevice device, VmaAllocator allocator,
                            VkCommandPool command_pool, VkQueue queue,
                            VkPipelineCache pipeline_cache) {
    device_ = device;
    vk_pipeline_cache_ = pipeline_cache;
    allocator_ = allocator;
    command_pool_ = command_pool;
    queue_ = queue;

    create_cubemap();
    create_cubemap_pipeline();
    create_brdf_lut();
    create_brdf_pipeline();
    generate_brdf_lut();

    FJELL_GFX_DEBUG("Sky cubemap pass created");
}

void SkyCubemapPass::post_create(GpuContext& gpu, PassRegistry& passes) {
    // Copy initial sky colors from the sky gradient pass
    auto& sky = passes.get<SkyPass>("sky");
    set_colors(sky.top_color(), sky.bottom_color());

    // One-shot cubemap generation
    VkCommandBuffer cmd{};
    VkCommandBufferAllocateInfo cmd_ai{};
    cmd_ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_ai.commandPool = command_pool_;
    cmd_ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_ai.commandBufferCount = 1;
    vkAllocateCommandBuffers(device_, &cmd_ai, &cmd);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    generate(cmd);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_);
    vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);

    // Write cubemap and BRDF LUT into global descriptors
    auto& gb = gpu.global_bindings();
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        gb.write_image(i, "sky_cubemap",
            cubemap_view_, cubemap_sampler_,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        gb.write_image(i, "brdf_lut",
            brdf_lut_view_, brdf_lut_sampler_,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        gb.flush_writes();
    }
}

void SkyCubemapPass::register_resources(ResourceRegistry& registry) {
    registry_ = &registry;
    registry.register_image("sky_cubemap",
        {.frame_graph_id = 0, .image = cubemap_image_, .view = cubemap_view(), .sampler = cubemap_sampler()});
}

void SkyCubemapPass::destroy() {
    if (device_ == VK_NULL_HANDLE) return;

    // BRDF compute
    if (brdf_pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device_, brdf_pipeline_, nullptr);
    if (brdf_pipeline_layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, brdf_pipeline_layout_, nullptr);
    if (brdf_pool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(device_, brdf_pool_, nullptr);
    if (brdf_set_layout_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device_, brdf_set_layout_, nullptr);

    // BRDF LUT image
    if (brdf_lut_sampler_ != VK_NULL_HANDLE) vkDestroySampler(device_, brdf_lut_sampler_, nullptr);
    if (brdf_lut_view_ != VK_NULL_HANDLE) vkDestroyImageView(device_, brdf_lut_view_, nullptr);
    destroy_vma_image(allocator_, brdf_lut_image_, brdf_lut_memory_);

    // Cubemap compute
    if (cubemap_pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device_, cubemap_pipeline_, nullptr);
    if (cubemap_pipeline_layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, cubemap_pipeline_layout_, nullptr);
    if (cubemap_pool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(device_, cubemap_pool_, nullptr);
    if (cubemap_set_layout_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device_, cubemap_set_layout_, nullptr);

    // Cubemap image
    if (cubemap_sampler_ != VK_NULL_HANDLE) vkDestroySampler(device_, cubemap_sampler_, nullptr);
    if (cubemap_storage_view_ != VK_NULL_HANDLE) vkDestroyImageView(device_, cubemap_storage_view_, nullptr);
    if (cubemap_view_ != VK_NULL_HANDLE) vkDestroyImageView(device_, cubemap_view_, nullptr);
    destroy_vma_image(allocator_, cubemap_image_, cubemap_memory_);

    destroy_hdri();

    device_ = VK_NULL_HANDLE;
}

void SkyCubemapPass::prepare_frame(const FrameContext& ctx, GpuContext& gpu) {
    if (!ctx.scene) return;

    // Pull the sun direction / color from the first directional light in the
    // scene so the atmospheric cubemap matches the sky pass and the direct
    // lighting term. Falls back to straight-up sun if none exists.
    glm::vec3 sun_dir{0.0f, 1.0f, 0.0f};
    glm::vec3 sun_color{1.0f};
    for (const auto& obj : ctx.scene->objects()) {
        if (const auto* dl = dynamic_cast<const DirectionalLightNode*>(obj.get())) {
            sun_dir = glm::normalize(-dl->direction);
            sun_color = dl->color;
            break;
        }
    }

    for (const auto& obj : ctx.scene->objects()) {
        if (const auto* env = dynamic_cast<const WorldEnvironmentNode*>(obj.get())) {
            set_colors(env->sky_top_color, env->sky_bottom_color);
            set_atmosphere(env->atmosphere_enabled, sun_dir, sun_color,
                           env->atmosphere_sun_intensity,
                           env->atmosphere_rayleigh_scale,
                           env->atmosphere_mie_scale,
                           env->atmosphere_mie_g,
                           env->atmosphere_mie_coeff);

            // Load HDRI if path changed (works regardless of atmosphere mode —
            // atmosphere modulates the HDRI color rather than replacing it).
            if (!env->hdri_path.empty() && env->hdri_path != loaded_hdri_path_) {
                auto resolved = base_path::resolve_asset(env->hdri_path, project_);
                load_hdri(resolved);
                loaded_hdri_path_ = env->hdri_path;

                // Update global descriptor binding to point to HDRI cubemap
                auto& gb = gpu.global_bindings();
                for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
                    gb.write_image(i, "sky_cubemap",
                        hdri_cubemap_view_, hdri_cubemap_sampler_,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    gb.flush_writes();
                }

                // Update ResourceRegistry + bump version so consumers (DDGI)
                // rewrite their cached sky_cubemap descriptor.
                if (registry_ != nullptr) {
                    registry_->register_image("sky_cubemap",
                        {.frame_graph_id = 0, .image = hdri_cubemap_image_,
                         .view = hdri_cubemap_view_, .sampler = hdri_cubemap_sampler_});
                }
                ++binding_version_;
            } else if (env->hdri_path.empty() && !loaded_hdri_path_.empty()) {
                // HDRI was cleared — revert to gradient cubemap.
                // The sky_cubemap binding (on both the global set and DDGI's
                // RT set) isn't UPDATE_AFTER_BIND, so we must wait for
                // in-flight frames before rewriting the descriptor and
                // destroying the HDRI image/sampler it referenced.
                vkDeviceWaitIdle(device_);

                auto& gb = gpu.global_bindings();
                for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
                    gb.write_image(i, "sky_cubemap",
                        cubemap_view_, cubemap_sampler_,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    gb.flush_writes();
                }

                if (registry_ != nullptr) {
                    registry_->register_image("sky_cubemap",
                        {.frame_graph_id = 0, .image = cubemap_image_,
                         .view = cubemap_view_, .sampler = cubemap_sampler_});
                }
                ++binding_version_;

                destroy_hdri();
                dirty_ = true;
            }
            break;
        }
    }
}

void SkyCubemapPass::set_colors(const glm::vec3& top, const glm::vec3& bottom) {
    if (top_color_ != top || bottom_color_ != bottom) {
        top_color_ = top;
        bottom_color_ = bottom;
        dirty_ = true;
    }
}

void SkyCubemapPass::set_atmosphere(bool enabled, const glm::vec3& sun_dir,
                                     const glm::vec3& sun_color, float sun_intensity,
                                     float rayleigh_scale, float mie_scale,
                                     float mie_g, float mie_coeff) {
    bool changed = enabled != atmosphere_enabled_;
    if (enabled) {
        if (sun_dir != atm_sun_dir_ || sun_color != atm_sun_color_ ||
            sun_intensity != atm_sun_intensity_ ||
            rayleigh_scale != atm_rayleigh_scale_ ||
            mie_scale != atm_mie_scale_ || mie_g != atm_mie_g_ ||
            mie_coeff != atm_mie_coeff_) {
            changed = true;
        }
    }
    atmosphere_enabled_ = enabled;
    atm_sun_dir_ = sun_dir;
    atm_sun_color_ = sun_color;
    atm_sun_intensity_ = sun_intensity;
    atm_rayleigh_scale_ = rayleigh_scale;
    atm_mie_scale_ = mie_scale;
    atm_mie_g_ = mie_g;
    atm_mie_coeff_ = mie_coeff;
    if (changed) dirty_ = true;
}

void SkyCubemapPass::generate(VkCommandBuffer cmd) {
    if (!dirty_) return;

    // Transition cubemap mip 0 to GENERAL for compute write
    {
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = first_generate_
            ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT
            : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.srcAccessMask = first_generate_
            ? 0
            : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        barrier.oldLayout = first_generate_
            ? VK_IMAGE_LAYOUT_UNDEFINED
            : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = cubemap_image_;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = MIP_LEVELS;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 6;

        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    // Dispatch cubemap compute shader
    struct PushConstants {
        glm::vec4 top_color;
        glm::vec4 bottom_color;
        glm::vec4 light_direction;  // xyz = sun dir, w unused
        glm::vec4 light_color;      // xyz = rgb, w unused
        uint32_t face_size;
        uint32_t sky_mode;          // 0 = gradient, 2 = atmosphere
        float atmosphere_rayleigh_scale;
        float atmosphere_mie_scale;
        float atmosphere_mie_g;
        float atmosphere_sun_intensity;
        float atmosphere_mie_coeff;
        float padding0;
    } push{
        glm::vec4(top_color_, 1.0f),
        glm::vec4(bottom_color_, 1.0f),
        glm::vec4(atm_sun_dir_, 0.0f),
        glm::vec4(atm_sun_color_, 0.0f),
        CUBEMAP_SIZE,
        atmosphere_enabled_ ? 2u : 0u,
        atm_rayleigh_scale_,
        atm_mie_scale_,
        atm_mie_g_,
        atm_sun_intensity_,
        atm_mie_coeff_,
        0.0f
    };

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cubemap_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            cubemap_pipeline_layout_, 0, 1, &cubemap_set_, 0, nullptr);
    vkCmdPushConstants(cmd, cubemap_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);

    uint32_t groups_xy = (CUBEMAP_SIZE + 7) / 8;
    vkCmdDispatch(cmd, groups_xy, groups_xy, 6);

    // Generate mips via vkCmdBlitImage
    generate_mips(cmd);

    dirty_ = false;
    first_generate_ = false;
}

void SkyCubemapPass::generate_mips(VkCommandBuffer cmd) {
    // After compute: mip 0 is in GENERAL layout. Transition to TRANSFER_SRC.
    {
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = cubemap_image_;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 6;

        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    int32_t mip_width = static_cast<int32_t>(CUBEMAP_SIZE);
    int32_t mip_height = static_cast<int32_t>(CUBEMAP_SIZE);

    for (uint32_t mip = 1; mip < MIP_LEVELS; ++mip) {
        int32_t next_width = std::max(mip_width / 2, 1);
        int32_t next_height = std::max(mip_height / 2, 1);

        // Transition dst mip to TRANSFER_DST
        {
            VkImageMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
            barrier.srcAccessMask = 0;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL; // was set to GENERAL earlier
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = cubemap_image_;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.baseMipLevel = mip;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.baseArrayLayer = 0;
            barrier.subresourceRange.layerCount = 6;

            VkDependencyInfo dep{};
            dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(cmd, &dep);
        }

        // Blit from previous mip to this mip
        VkImageBlit2 blit{};
        blit.sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2;
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel = mip - 1;
        blit.srcSubresource.baseArrayLayer = 0;
        blit.srcSubresource.layerCount = 6;
        blit.srcOffsets[0] = {0, 0, 0};
        blit.srcOffsets[1] = {mip_width, mip_height, 1};
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.mipLevel = mip;
        blit.dstSubresource.baseArrayLayer = 0;
        blit.dstSubresource.layerCount = 6;
        blit.dstOffsets[0] = {0, 0, 0};
        blit.dstOffsets[1] = {next_width, next_height, 1};

        VkBlitImageInfo2 blit_info{};
        blit_info.sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2;
        blit_info.srcImage = cubemap_image_;
        blit_info.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        blit_info.dstImage = cubemap_image_;
        blit_info.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        blit_info.regionCount = 1;
        blit_info.pRegions = &blit;
        blit_info.filter = VK_FILTER_LINEAR;

        vkCmdBlitImage2(cmd, &blit_info);

        // Transition this mip to TRANSFER_SRC for next iteration
        {
            VkImageMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = cubemap_image_;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.baseMipLevel = mip;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.baseArrayLayer = 0;
            barrier.subresourceRange.layerCount = 6;

            VkDependencyInfo dep{};
            dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(cmd, &dep);
        }

        mip_width = next_width;
        mip_height = next_height;
    }

    // Transition all mips from TRANSFER_SRC to SHADER_READ_ONLY_OPTIMAL
    {
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = cubemap_image_;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = MIP_LEVELS;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 6;

        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);
    }
}

void SkyCubemapPass::create_cubemap() {
    // Create cubemap image
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    ci.extent = {CUBEMAP_SIZE, CUBEMAP_SIZE, 1};
    ci.mipLevels = MIP_LEVELS;
    ci.arrayLayers = 6;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vk_check(create_vma_image(allocator_, &ci, &cubemap_image_, &cubemap_memory_),
             "Failed to create sky cubemap image");

    // Cube view for sampling (all mips, all layers)
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = cubemap_image_;
    vi.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    vi.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.baseMipLevel = 0;
    vi.subresourceRange.levelCount = MIP_LEVELS;
    vi.subresourceRange.baseArrayLayer = 0;
    vi.subresourceRange.layerCount = 6;
    vk_check(vkCreateImageView(device_, &vi, nullptr, &cubemap_view_),
             "Failed to create sky cubemap cube view");

    // 2D array view for compute storage writes (mip 0 only)
    VkImageViewCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    si.image = cubemap_image_;
    si.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    si.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    si.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    si.subresourceRange.baseMipLevel = 0;
    si.subresourceRange.levelCount = 1;
    si.subresourceRange.baseArrayLayer = 0;
    si.subresourceRange.layerCount = 6;
    vk_check(vkCreateImageView(device_, &si, nullptr, &cubemap_storage_view_),
             "Failed to create sky cubemap storage view");

    // Sampler with trilinear filtering for mip sampling
    VkSamplerCreateInfo samp{};
    samp.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samp.magFilter = VK_FILTER_LINEAR;
    samp.minFilter = VK_FILTER_LINEAR;
    samp.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samp.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samp.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samp.maxLod = static_cast<float>(MIP_LEVELS);
    vk_check(vkCreateSampler(device_, &samp, nullptr, &cubemap_sampler_),
             "Failed to create sky cubemap sampler");
}

void SkyCubemapPass::create_brdf_lut() {
    // Create BRDF LUT image
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R16G16_SFLOAT;
    ci.extent = {BRDF_LUT_SIZE, BRDF_LUT_SIZE, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vk_check(create_vma_image(allocator_, &ci, &brdf_lut_image_, &brdf_lut_memory_),
             "Failed to create BRDF LUT image");

    // Image view
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = brdf_lut_image_;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R16G16_SFLOAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk_check(vkCreateImageView(device_, &vi, nullptr, &brdf_lut_view_),
             "Failed to create BRDF LUT view");

    // Sampler
    VkSamplerCreateInfo samp{};
    samp.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samp.magFilter = VK_FILTER_LINEAR;
    samp.minFilter = VK_FILTER_LINEAR;
    samp.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samp.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vk_check(vkCreateSampler(device_, &samp, nullptr, &brdf_lut_sampler_),
             "Failed to create BRDF LUT sampler");
}

void SkyCubemapPass::create_cubemap_pipeline() {
    // Descriptor set layout: binding 0 = storage image (2D array)
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_ci.bindingCount = 1;
    layout_ci.pBindings = &binding;
    vk_check(vkCreateDescriptorSetLayout(device_, &layout_ci, nullptr, &cubemap_set_layout_),
             "Failed to create cubemap descriptor set layout");

    // Descriptor pool
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    pool_size.descriptorCount = 1;

    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.maxSets = 1;
    pool_ci.poolSizeCount = 1;
    pool_ci.pPoolSizes = &pool_size;
    vk_check(vkCreateDescriptorPool(device_, &pool_ci, nullptr, &cubemap_pool_),
             "Failed to create cubemap descriptor pool");

    // Allocate descriptor set
    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = cubemap_pool_;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &cubemap_set_layout_;
    vk_check(vkAllocateDescriptorSets(device_, &alloc_info, &cubemap_set_),
             "Failed to allocate cubemap descriptor set");

    // Write storage image descriptor
    VkDescriptorImageInfo img_info{};
    img_info.imageView = cubemap_storage_view_;
    img_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = cubemap_set_;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.descriptorCount = 1;
    write.pImageInfo = &img_info;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    // Push constants: top_color (vec4) + bottom_color (vec4) + face_size (uint)
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset = 0;
    // top + bottom + light_dir + light_color = 4 vec4 = 64 bytes
    // face_size (u32) + sky_mode (u32) + 6 floats + padding = 32 bytes
    push_range.size = sizeof(glm::vec4) * 4 + 8 * sizeof(uint32_t);

    // Pipeline layout
    VkPipelineLayoutCreateInfo pl_ci{};
    pl_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl_ci.setLayoutCount = 1;
    pl_ci.pSetLayouts = &cubemap_set_layout_;
    pl_ci.pushConstantRangeCount = 1;
    pl_ci.pPushConstantRanges = &push_range;
    vk_check(vkCreatePipelineLayout(device_, &pl_ci, nullptr, &cubemap_pipeline_layout_),
             "Failed to create cubemap pipeline layout");

    // Compute pipeline
    auto code = read_spirv(base_path::resolve("shaders/sky_cubemap.comp.spv"));
    auto module = create_shader_module(device_, code);

    VkComputePipelineCreateInfo comp_ci{};
    comp_ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    comp_ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    comp_ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    comp_ci.stage.module = module;
    comp_ci.stage.pName = "main";
    comp_ci.layout = cubemap_pipeline_layout_;
    vk_check(vkCreateComputePipelines(device_, vk_pipeline_cache_, 1, &comp_ci,
                                       nullptr, &cubemap_pipeline_),
             "Failed to create cubemap compute pipeline");

    vkDestroyShaderModule(device_, module, nullptr);
}

void SkyCubemapPass::create_brdf_pipeline() {
    // Descriptor set layout: binding 0 = storage image (2D)
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_ci.bindingCount = 1;
    layout_ci.pBindings = &binding;
    vk_check(vkCreateDescriptorSetLayout(device_, &layout_ci, nullptr, &brdf_set_layout_),
             "Failed to create BRDF descriptor set layout");

    // Descriptor pool
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    pool_size.descriptorCount = 1;

    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.maxSets = 1;
    pool_ci.poolSizeCount = 1;
    pool_ci.pPoolSizes = &pool_size;
    vk_check(vkCreateDescriptorPool(device_, &pool_ci, nullptr, &brdf_pool_),
             "Failed to create BRDF descriptor pool");

    // Allocate descriptor set
    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = brdf_pool_;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &brdf_set_layout_;
    vk_check(vkAllocateDescriptorSets(device_, &alloc_info, &brdf_set_),
             "Failed to allocate BRDF descriptor set");

    // Write storage image descriptor
    VkDescriptorImageInfo img_info{};
    img_info.imageView = brdf_lut_view_;
    img_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = brdf_set_;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.descriptorCount = 1;
    write.pImageInfo = &img_info;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    // Pipeline layout (no push constants for BRDF)
    VkPipelineLayoutCreateInfo pl_ci{};
    pl_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl_ci.setLayoutCount = 1;
    pl_ci.pSetLayouts = &brdf_set_layout_;
    vk_check(vkCreatePipelineLayout(device_, &pl_ci, nullptr, &brdf_pipeline_layout_),
             "Failed to create BRDF pipeline layout");

    // Compute pipeline
    auto code = read_spirv(base_path::resolve("shaders/brdf_lut.comp.spv"));
    auto module = create_shader_module(device_, code);

    VkComputePipelineCreateInfo comp_ci{};
    comp_ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    comp_ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    comp_ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    comp_ci.stage.module = module;
    comp_ci.stage.pName = "main";
    comp_ci.layout = brdf_pipeline_layout_;
    vk_check(vkCreateComputePipelines(device_, vk_pipeline_cache_, 1, &comp_ci,
                                       nullptr, &brdf_pipeline_),
             "Failed to create BRDF compute pipeline");

    vkDestroyShaderModule(device_, module, nullptr);
}

void SkyCubemapPass::generate_brdf_lut() {
    // One-shot command buffer to generate the BRDF LUT at startup
    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo cmd_ai{};
    cmd_ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_ai.commandPool = command_pool_;
    cmd_ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_ai.commandBufferCount = 1;
    vk_check(vkAllocateCommandBuffers(device_, &cmd_ai, &cmd),
             "Failed to allocate BRDF LUT command buffer");

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(cmd, &begin),
             "Failed to begin BRDF LUT command buffer");

    // Transition BRDF LUT to GENERAL for compute write
    {
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        barrier.srcAccessMask = 0;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = brdf_lut_image_;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    // Dispatch BRDF LUT compute
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, brdf_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            brdf_pipeline_layout_, 0, 1, &brdf_set_, 0, nullptr);

    uint32_t groups = (BRDF_LUT_SIZE + 7) / 8;
    vkCmdDispatch(cmd, groups, groups, 1);

    // Transition BRDF LUT to SHADER_READ_ONLY
    {
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = brdf_lut_image_;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    vk_check(vkEndCommandBuffer(cmd),
             "Failed to end BRDF LUT command buffer");

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vk_check(vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE),
             "Failed to submit BRDF LUT generation");
    vkQueueWaitIdle(queue_);
    vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);

    FJELL_GFX_DEBUG("BRDF LUT generated ({}x{})", BRDF_LUT_SIZE, BRDF_LUT_SIZE);
}

void SkyCubemapPass::record(const FrameContext& ctx) {
    if (ctx.passes != nullptr) {
        auto& sky = ctx.passes->get<SkyPass>("sky");
        set_colors(sky.top_color(), sky.bottom_color());
    }
    if (dirty()) {
        FJELL_GPU_ZONE(ctx, "Sky Cubemap");
        generate(ctx.cmd);
    }
}

PassSetup SkyCubemapPass::setup() const {
    return {
        .name = "sky_cubemap",
        .phase = RenderPhase::Environment,
        .sub_order = 50,
    };
}

void SkyCubemapPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    auto cubemap = builder.import_named(ctx, "sky_cubemap");
    if (cubemap.valid()) {
        // SkyCubemapPass manages its own mip-chain layout choreography
        // inside generate() — the graph would otherwise emit a barrier
        // for a write that only happens when dirty_, leaving the
        // cubemap in GENERAL on clean frames. Declaring only the
        // final_layout lets the graph track the post-state without
        // pre-transitioning the image.
        builder.final_layout(cubemap,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    }
    // BRDF LUT is a static pass-private texture — not in the catalog.
}

void SkyCubemapPass::collect_exports(DeclareContext& ctx) {
    // HDRI takes precedence over the procedural cubemap when present.
    VkImage image = hdri_cubemap_image_ != VK_NULL_HANDLE
        ? hdri_cubemap_image_ : cubemap_image_;
    VkImageView view = hdri_cubemap_view_ != VK_NULL_HANDLE
        ? hdri_cubemap_view_ : cubemap_view_;
    if (image == VK_NULL_HANDLE || view == VK_NULL_HANDLE) { return; }
    const uint32_t mips = hdri_cubemap_view_ != VK_NULL_HANDLE
        ? HDRI_MIP_LEVELS : 1;
    ImportedImage img{};
    img.image = image;
    img.view = view;
    img.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    img.base_layer = 0;
    img.layer_count = 6;
    img.mip_count = mips;
    img.persistent = true;
    ctx.imports.emplace("sky_cubemap", img);
}

// ── HDRI environment map support ─────────────────────────────────────────

void SkyCubemapPass::create_hdri_pipelines() {
    if (hdri_pipelines_created_) return;

    // ── Equirect-to-cube pipeline ─────────────────────────────────────
    // Layout: binding 0 = equirect sampler, binding 1 = cubemap storage
    VkDescriptorSetLayoutBinding eq_bindings[2]{};
    eq_bindings[0].binding = 0;
    eq_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    eq_bindings[0].descriptorCount = 1;
    eq_bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    eq_bindings[1].binding = 1;
    eq_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    eq_bindings[1].descriptorCount = 1;
    eq_bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo eq_layout_ci{};
    eq_layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    eq_layout_ci.bindingCount = 2;
    eq_layout_ci.pBindings = eq_bindings;
    vkCreateDescriptorSetLayout(device_, &eq_layout_ci, nullptr, &equirect_set_layout_);

    VkPushConstantRange eq_push{};
    eq_push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    eq_push.size = sizeof(uint32_t);

    VkPipelineLayoutCreateInfo eq_pipe_ci{};
    eq_pipe_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    eq_pipe_ci.setLayoutCount = 1;
    eq_pipe_ci.pSetLayouts = &equirect_set_layout_;
    eq_pipe_ci.pushConstantRangeCount = 1;
    eq_pipe_ci.pPushConstantRanges = &eq_push;
    vkCreatePipelineLayout(device_, &eq_pipe_ci, nullptr, &equirect_pipeline_layout_);

    auto eq_code = read_spirv(base_path::resolve("shaders/equirect_to_cube.comp.spv"));
    auto eq_mod = create_shader_module(device_, eq_code);
    VkPipelineShaderStageCreateInfo eq_stage{};
    eq_stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    eq_stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    eq_stage.module = eq_mod;
    eq_stage.pName = "main";
    VkComputePipelineCreateInfo eq_pipe{};
    eq_pipe.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    eq_pipe.stage = eq_stage;
    eq_pipe.layout = equirect_pipeline_layout_;
    vkCreateComputePipelines(device_, vk_pipeline_cache_, 1, &eq_pipe, nullptr, &equirect_pipeline_);
    vkDestroyShaderModule(device_, eq_mod, nullptr);

    // Descriptor pool for equirect (1 set: 1 sampler + 1 storage)
    VkDescriptorPoolSize eq_pool_sizes[2]{};
    eq_pool_sizes[0] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    eq_pool_sizes[1] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1};
    VkDescriptorPoolCreateInfo eq_pool_ci{};
    eq_pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    eq_pool_ci.maxSets = 1;
    eq_pool_ci.poolSizeCount = 2;
    eq_pool_ci.pPoolSizes = eq_pool_sizes;
    vkCreateDescriptorPool(device_, &eq_pool_ci, nullptr, &equirect_pool_);

    VkDescriptorSetAllocateInfo eq_alloc{};
    eq_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    eq_alloc.descriptorPool = equirect_pool_;
    eq_alloc.descriptorSetCount = 1;
    eq_alloc.pSetLayouts = &equirect_set_layout_;
    vkAllocateDescriptorSets(device_, &eq_alloc, &equirect_set_);

    // ── Prefilter pipeline ───────────────────────────────────────────
    // Layout: binding 0 = source cubemap sampler, binding 1 = output storage
    VkDescriptorSetLayoutBinding pf_bindings[2]{};
    pf_bindings[0].binding = 0;
    pf_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pf_bindings[0].descriptorCount = 1;
    pf_bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pf_bindings[1].binding = 1;
    pf_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    pf_bindings[1].descriptorCount = 1;
    pf_bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo pf_layout_ci{};
    pf_layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    pf_layout_ci.bindingCount = 2;
    pf_layout_ci.pBindings = pf_bindings;
    vkCreateDescriptorSetLayout(device_, &pf_layout_ci, nullptr, &prefilter_set_layout_);

    struct PrefilterPush { uint32_t face_size; float roughness; uint32_t sample_count; };
    VkPushConstantRange pf_push{};
    pf_push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pf_push.size = sizeof(PrefilterPush);

    VkPipelineLayoutCreateInfo pf_pipe_ci{};
    pf_pipe_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pf_pipe_ci.setLayoutCount = 1;
    pf_pipe_ci.pSetLayouts = &prefilter_set_layout_;
    pf_pipe_ci.pushConstantRangeCount = 1;
    pf_pipe_ci.pPushConstantRanges = &pf_push;
    vkCreatePipelineLayout(device_, &pf_pipe_ci, nullptr, &prefilter_pipeline_layout_);

    auto pf_code = read_spirv(base_path::resolve("shaders/prefilter_envmap.comp.spv"));
    auto pf_mod = create_shader_module(device_, pf_code);
    VkPipelineShaderStageCreateInfo pf_stage{};
    pf_stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pf_stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pf_stage.module = pf_mod;
    pf_stage.pName = "main";
    VkComputePipelineCreateInfo pf_pipe{};
    pf_pipe.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pf_pipe.stage = pf_stage;
    pf_pipe.layout = prefilter_pipeline_layout_;
    vkCreateComputePipelines(device_, vk_pipeline_cache_, 1, &pf_pipe, nullptr, &prefilter_pipeline_);
    vkDestroyShaderModule(device_, pf_mod, nullptr);

    // Descriptor pool for prefilter (HDRI_MIP_LEVELS sets: each has 1 sampler + 1 storage)
    VkDescriptorPoolSize pf_pool_sizes[2]{};
    pf_pool_sizes[0] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, HDRI_MIP_LEVELS};
    pf_pool_sizes[1] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, HDRI_MIP_LEVELS};
    VkDescriptorPoolCreateInfo pf_pool_ci{};
    pf_pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pf_pool_ci.maxSets = HDRI_MIP_LEVELS;
    pf_pool_ci.poolSizeCount = 2;
    pf_pool_ci.pPoolSizes = pf_pool_sizes;
    vkCreateDescriptorPool(device_, &pf_pool_ci, nullptr, &prefilter_pool_);

    std::array<VkDescriptorSetLayout, HDRI_MIP_LEVELS> pf_layouts;
    pf_layouts.fill(prefilter_set_layout_);
    VkDescriptorSetAllocateInfo pf_alloc{};
    pf_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    pf_alloc.descriptorPool = prefilter_pool_;
    pf_alloc.descriptorSetCount = HDRI_MIP_LEVELS;
    pf_alloc.pSetLayouts = pf_layouts.data();
    vkAllocateDescriptorSets(device_, &pf_alloc, prefilter_sets_.data());

    hdri_pipelines_created_ = true;
    FJELL_GFX_DEBUG("HDRI pipelines created");
}

void SkyCubemapPass::destroy_hdri() {
    // Per-mip views
    for (auto& v : hdri_mip_views_) {
        if (v != VK_NULL_HANDLE) { vkDestroyImageView(device_, v, nullptr); v = VK_NULL_HANDLE; }
    }

    // HDRI cubemap
    if (hdri_cubemap_sampler_ != VK_NULL_HANDLE) { vkDestroySampler(device_, hdri_cubemap_sampler_, nullptr); hdri_cubemap_sampler_ = VK_NULL_HANDLE; }
    if (hdri_storage_view_ != VK_NULL_HANDLE) { vkDestroyImageView(device_, hdri_storage_view_, nullptr); hdri_storage_view_ = VK_NULL_HANDLE; }
    if (hdri_cubemap_view_ != VK_NULL_HANDLE) { vkDestroyImageView(device_, hdri_cubemap_view_, nullptr); hdri_cubemap_view_ = VK_NULL_HANDLE; }
    if (hdri_cubemap_image_ != VK_NULL_HANDLE) { destroy_vma_image(allocator_, hdri_cubemap_image_, hdri_cubemap_memory_); hdri_cubemap_image_ = VK_NULL_HANDLE; }

    // Equirect source
    if (equirect_sampler_ != VK_NULL_HANDLE) { vkDestroySampler(device_, equirect_sampler_, nullptr); equirect_sampler_ = VK_NULL_HANDLE; }
    if (equirect_view_ != VK_NULL_HANDLE) { vkDestroyImageView(device_, equirect_view_, nullptr); equirect_view_ = VK_NULL_HANDLE; }
    if (equirect_image_ != VK_NULL_HANDLE) { destroy_vma_image(allocator_, equirect_image_, equirect_memory_); equirect_image_ = VK_NULL_HANDLE; }

    // Pipelines (keep for reuse)
    // Pools/sets freed with pool

    if (equirect_pipeline_ != VK_NULL_HANDLE) { vkDestroyPipeline(device_, equirect_pipeline_, nullptr); equirect_pipeline_ = VK_NULL_HANDLE; }
    if (equirect_pipeline_layout_ != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device_, equirect_pipeline_layout_, nullptr); equirect_pipeline_layout_ = VK_NULL_HANDLE; }
    if (equirect_pool_ != VK_NULL_HANDLE) { vkDestroyDescriptorPool(device_, equirect_pool_, nullptr); equirect_pool_ = VK_NULL_HANDLE; }
    if (equirect_set_layout_ != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device_, equirect_set_layout_, nullptr); equirect_set_layout_ = VK_NULL_HANDLE; }

    if (prefilter_pipeline_ != VK_NULL_HANDLE) { vkDestroyPipeline(device_, prefilter_pipeline_, nullptr); prefilter_pipeline_ = VK_NULL_HANDLE; }
    if (prefilter_pipeline_layout_ != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device_, prefilter_pipeline_layout_, nullptr); prefilter_pipeline_layout_ = VK_NULL_HANDLE; }
    if (prefilter_pool_ != VK_NULL_HANDLE) { vkDestroyDescriptorPool(device_, prefilter_pool_, nullptr); prefilter_pool_ = VK_NULL_HANDLE; }
    if (prefilter_set_layout_ != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device_, prefilter_set_layout_, nullptr); prefilter_set_layout_ = VK_NULL_HANDLE; }

    hdri_pipelines_created_ = false;
    loaded_hdri_path_.clear();
}

void SkyCubemapPass::load_hdri(const std::string& path) {
    if (path == loaded_hdri_path_ && hdri_cubemap_view_ != VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(device_);
    destroy_hdri();
    create_hdri_pipelines();

    // ── 1. Load equirectangular HDR via stb ────────────────────────────
    int width = 0, height = 0, channels = 0;
    float* pixels = stbi_loadf(path.c_str(), &width, &height, &channels, 4);
    if (!pixels) {
        FJELL_GFX_ERROR("Failed to load HDRI: {}", path);
        return;
    }
    FJELL_GFX_INFO("Loaded HDRI: {}x{} from {}", width, height, path);

    VkDeviceSize image_size = static_cast<VkDeviceSize>(width) * height * 4 * sizeof(float);

    // Staging buffer
    VkBufferCreateInfo staging_ci{};
    staging_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    staging_ci.size = image_size;
    staging_ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo staging_alloc_ci{};
    staging_alloc_ci.usage = VMA_MEMORY_USAGE_AUTO;
    staging_alloc_ci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;

    VkBuffer staging_buf = VK_NULL_HANDLE;
    VmaAllocation staging_mem = VK_NULL_HANDLE;
    vmaCreateBuffer(allocator_, &staging_ci, &staging_alloc_ci, &staging_buf, &staging_mem, nullptr);

    void* mapped = nullptr;
    vmaMapMemory(allocator_, staging_mem, &mapped);
    std::memcpy(mapped, pixels, image_size);
    vmaUnmapMemory(allocator_, staging_mem);
    stbi_image_free(pixels);

    // Create equirect image (R32G32B32A32_SFLOAT, 2D)
    VkImageCreateInfo eq_ci{};
    eq_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    eq_ci.imageType = VK_IMAGE_TYPE_2D;
    eq_ci.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    eq_ci.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
    eq_ci.mipLevels = 1;
    eq_ci.arrayLayers = 1;
    eq_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    eq_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    eq_ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    eq_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vk_check(create_vma_image(allocator_, &eq_ci, &equirect_image_, &equirect_memory_),
             "Failed to create equirect image");

    VkImageViewCreateInfo eq_vi{};
    eq_vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    eq_vi.image = equirect_image_;
    eq_vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    eq_vi.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    eq_vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(device_, &eq_vi, nullptr, &equirect_view_);

    VkSamplerCreateInfo eq_samp{};
    eq_samp.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    eq_samp.magFilter = VK_FILTER_LINEAR;
    eq_samp.minFilter = VK_FILTER_LINEAR;
    eq_samp.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    eq_samp.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    eq_samp.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    vkCreateSampler(device_, &eq_samp, nullptr, &equirect_sampler_);

    // ── 2. Create HDRI cubemap (512x512, 10 mips) ─────────────────────
    VkImageCreateInfo cube_ci{};
    cube_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    cube_ci.imageType = VK_IMAGE_TYPE_2D;
    cube_ci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    cube_ci.extent = {HDRI_CUBEMAP_SIZE, HDRI_CUBEMAP_SIZE, 1};
    cube_ci.mipLevels = HDRI_MIP_LEVELS;
    cube_ci.arrayLayers = 6;
    cube_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    cube_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    cube_ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    cube_ci.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    cube_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vk_check(create_vma_image(allocator_, &cube_ci, &hdri_cubemap_image_, &hdri_cubemap_memory_),
             "Failed to create HDRI cubemap");

    // Cube view (all mips, for sampling)
    VkImageViewCreateInfo cube_vi{};
    cube_vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    cube_vi.image = hdri_cubemap_image_;
    cube_vi.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    cube_vi.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    cube_vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, HDRI_MIP_LEVELS, 0, 6};
    vkCreateImageView(device_, &cube_vi, nullptr, &hdri_cubemap_view_);

    // 2D array view for mip 0 (equirect conversion output)
    VkImageViewCreateInfo storage_vi{};
    storage_vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    storage_vi.image = hdri_cubemap_image_;
    storage_vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    storage_vi.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    storage_vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
    vkCreateImageView(device_, &storage_vi, nullptr, &hdri_storage_view_);

    // Per-mip 2D array views for prefilter output
    for (uint32_t mip = 0; mip < HDRI_MIP_LEVELS; ++mip) {
        VkImageViewCreateInfo mip_vi{};
        mip_vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        mip_vi.image = hdri_cubemap_image_;
        mip_vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        mip_vi.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        mip_vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 6};
        vkCreateImageView(device_, &mip_vi, nullptr, &hdri_mip_views_[mip]);
    }

    // Sampler (trilinear for prefiltered mip sampling)
    VkSamplerCreateInfo cube_samp{};
    cube_samp.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    cube_samp.magFilter = VK_FILTER_LINEAR;
    cube_samp.minFilter = VK_FILTER_LINEAR;
    cube_samp.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    cube_samp.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    cube_samp.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    cube_samp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    cube_samp.maxLod = static_cast<float>(HDRI_MIP_LEVELS);
    vkCreateSampler(device_, &cube_samp, nullptr, &hdri_cubemap_sampler_);

    // ── 3. GPU work: upload + convert + prefilter ─────────────────────
    VkCommandBuffer cmd{};
    VkCommandBufferAllocateInfo cmd_ai{};
    cmd_ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_ai.commandPool = command_pool_;
    cmd_ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_ai.commandBufferCount = 1;
    vkAllocateCommandBuffers(device_, &cmd_ai, &cmd);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    // Upload equirect: transition + copy
    {
        VkImageMemoryBarrier2 b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = equirect_image_;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
    vkCmdCopyBufferToImage(cmd, staging_buf, equirect_image_,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Transition equirect to SHADER_READ_ONLY
    {
        VkImageMemoryBarrier2 b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = equirect_image_;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    // Transition cubemap mip 0 to GENERAL for compute write
    {
        VkImageMemoryBarrier2 b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = hdri_cubemap_image_;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, HDRI_MIP_LEVELS, 0, 6};
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    // Write equirect descriptor set
    {
        VkDescriptorImageInfo eq_info{};
        eq_info.sampler = equirect_sampler_;
        eq_info.imageView = equirect_view_;
        eq_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo cube_info{};
        cube_info.imageView = hdri_storage_view_;
        cube_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = equirect_set_;
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].descriptorCount = 1;
        writes[0].pImageInfo = &eq_info;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = equirect_set_;
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo = &cube_info;
        vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
    }

    // Dispatch equirect-to-cube conversion
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, equirect_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, equirect_pipeline_layout_,
                            0, 1, &equirect_set_, 0, nullptr);
    uint32_t face_size = HDRI_CUBEMAP_SIZE;
    vkCmdPushConstants(cmd, equirect_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(uint32_t), &face_size);
    uint32_t groups = (HDRI_CUBEMAP_SIZE + 7) / 8;
    vkCmdDispatch(cmd, groups, groups, 6);

    // Barrier: equirect conversion done → prefilter reads
    {
        VkMemoryBarrier2 b{};
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        b.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    // Transition mip 0 to SHADER_READ_ONLY so the prefilter can sample the unfiltered cubemap
    // while writing to higher mips (which stay GENERAL).
    // Actually — the prefilter samples via samplerCube which needs SHADER_READ_ONLY,
    // but writes to per-mip storage views which need GENERAL. Since all mips are already
    // GENERAL, and the cubemap sampler reads from mip 0 while writing to other mips,
    // we can keep everything in GENERAL — the driver handles coherency via the barrier above.

    // Write prefilter descriptor sets (one per mip level)
    for (uint32_t mip = 0; mip < HDRI_MIP_LEVELS; ++mip) {
        VkDescriptorImageInfo src_info{};
        src_info.sampler = hdri_cubemap_sampler_;
        src_info.imageView = hdri_cubemap_view_;
        src_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkDescriptorImageInfo dst_info{};
        dst_info.imageView = hdri_mip_views_[mip];
        dst_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = prefilter_sets_[mip];
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].descriptorCount = 1;
        writes[0].pImageInfo = &src_info;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = prefilter_sets_[mip];
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo = &dst_info;
        vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
    }

    // Dispatch prefilter for each mip level
    struct PrefilterPush { uint32_t face_size; float roughness; uint32_t sample_count; };

    // Skip mip 0 — equirect conversion already wrote unfiltered data there.
    // Prefiltering mip 0 would read+write the same subresource (undefined behavior).
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, prefilter_pipeline_);
    for (uint32_t mip = 1; mip < HDRI_MIP_LEVELS; ++mip) {
        uint32_t mip_size = std::max(HDRI_CUBEMAP_SIZE >> mip, 1u);
        float roughness = static_cast<float>(mip) / static_cast<float>(HDRI_MIP_LEVELS - 1);
        uint32_t samples = std::min(1024u, 64u * mip);

        PrefilterPush push{mip_size, roughness, samples};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, prefilter_pipeline_layout_,
                                0, 1, &prefilter_sets_[mip], 0, nullptr);
        vkCmdPushConstants(cmd, prefilter_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(push), &push);

        uint32_t g = (mip_size + 7) / 8;
        vkCmdDispatch(cmd, g, g, 6);

        // Barrier between mip levels
        if (mip + 1 < HDRI_MIP_LEVELS) {
            VkMemoryBarrier2 b{};
            b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
            b.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            b.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            VkDependencyInfo dep{};
            dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.memoryBarrierCount = 1;
            dep.pMemoryBarriers = &b;
            vkCmdPipelineBarrier2(cmd, &dep);
        }
    }

    // Final transition: all mips to SHADER_READ_ONLY_OPTIMAL
    {
        VkImageMemoryBarrier2 b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = hdri_cubemap_image_;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, HDRI_MIP_LEVELS, 0, 6};
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_);
    vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);

    // Clean up staging
    vmaDestroyBuffer(allocator_, staging_buf, staging_mem);

    FJELL_GFX_INFO("HDRI cubemap created: {}x{}, {} mips, prefiltered", HDRI_CUBEMAP_SIZE, HDRI_CUBEMAP_SIZE, HDRI_MIP_LEVELS);
}

} // namespace fjell
