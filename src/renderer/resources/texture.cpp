#include "renderer/resources/texture.hpp"
#include "renderer/gpu/upload_context.hpp"
#include "renderer/gpu/vk_check.hpp"
#include "core/log.hpp"

#include <stb_image.h>

#include <cmath>
#include <stdexcept>

namespace fjell {

Texture::Texture(VkDevice device, VmaAllocator allocator,
                 VkPhysicalDevice physical_device, UploadContext& upload_ctx,
                 const std::string& path, bool nearest)
    : device_{device} {
    int width, height, channels;
    stbi_uc* pixels = stbi_load(path.c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (!pixels) {
        throw std::runtime_error("Failed to load texture: " + path);
    }

    // Pixel art: no mipmaps, nearest filter, clamp to edge
    uint32_t mip = nearest ? 1 : 0;
    bool clamp = nearest;
    upload(allocator, physical_device, upload_ctx, pixels,
           static_cast<uint32_t>(width), static_cast<uint32_t>(height),
           VK_FORMAT_R8G8B8A8_SRGB, mip, clamp, nearest);
    stbi_image_free(pixels);

    FJELL_GFX_DEBUG("Texture loaded from file: {}x{} nearest={}", width, height, nearest);
}

Texture::Texture(VkDevice device, VmaAllocator allocator,
                 VkPhysicalDevice physical_device, UploadContext& upload_ctx,
                 const uint8_t* pixels, uint32_t width, uint32_t height,
                 VkFormat format, uint32_t mip_levels, bool clamp_to_edge, bool nearest)
    : device_{device} {
    upload(allocator, physical_device, upload_ctx, pixels, width, height, format, mip_levels, clamp_to_edge, nearest);

    FJELL_GFX_DEBUG("Texture loaded from memory: {}x{}", width, height);
}

Texture::~Texture() {
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, sampler_, nullptr);
    }
}

void Texture::upload(VmaAllocator allocator, VkPhysicalDevice physical_device,
                     UploadContext& upload_ctx,
                     const void* pixels, uint32_t width, uint32_t height,
                     VkFormat format, uint32_t mip_levels_override, bool clamp_to_edge,
                     bool nearest) {
    VkDeviceSize image_size = static_cast<VkDeviceSize>(width) * height * 4;
    uint32_t mip_levels = mip_levels_override > 0 ? mip_levels_override :
        static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1;

    StagingSlice staging = upload_ctx.stage_for_image(pixels, image_size);

    image_ = std::make_unique<Image>(
        allocator, width, height,
        format, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT,
        VMA_MEMORY_USAGE_AUTO, mip_levels);

    // Recorded into the upload batch; the flush at frame submission runs
    // this before any rendering on the same queue, so the image is
    // SHADER_READ_ONLY by the time anything samples it.
    VkCommandBuffer cmd = upload_ctx.image_cb();

    image_->transition_layout(cmd, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, mip_levels);

    VkBufferImageCopy region{};
    region.bufferOffset = staging.offset;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {width, height, 1};

    vkCmdCopyBufferToImage(cmd, staging.buffer, image_->handle(),
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    if (mip_levels > 1) {
        image_->generate_mipmaps(cmd, physical_device, width, height, mip_levels);
    } else {
        image_->transition_layout(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1);
    }

    image_->create_view(device_, format, VK_IMAGE_ASPECT_COLOR_BIT, mip_levels);

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical_device, &props);

    VkFilter filter = nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;

    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = filter;
    sampler_info.minFilter = filter;
    VkSamplerAddressMode addr_mode = clamp_to_edge
        ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
        : VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.addressModeU = addr_mode;
    sampler_info.addressModeV = addr_mode;
    sampler_info.addressModeW = addr_mode;
    sampler_info.anisotropyEnable = (clamp_to_edge || nearest) ? VK_FALSE : VK_TRUE;
    sampler_info.maxAnisotropy = (clamp_to_edge || nearest) ? 1.0f : props.limits.maxSamplerAnisotropy;
    sampler_info.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    sampler_info.unnormalizedCoordinates = VK_FALSE;
    sampler_info.compareEnable = VK_FALSE;
    sampler_info.mipmapMode = nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.minLod = 0.0f;
    sampler_info.maxLod = static_cast<float>(mip_levels);
    sampler_info.mipLodBias = 0.0f;

    if (vkCreateSampler(device_, &sampler_info, nullptr, &sampler_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create texture sampler");
    }
}

} // namespace fjell
