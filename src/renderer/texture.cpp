#include "renderer/texture.hpp"
#include "renderer/buffer.hpp"
#include "renderer/vk_check.hpp"
#include "core/log.hpp"

#include <stb_image.h>

#include <cmath>
#include <stdexcept>

namespace fjell {

Texture::Texture(VkDevice device, VkPhysicalDevice physical_device,
                 VkCommandPool command_pool, VkQueue queue,
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
    upload(physical_device, command_pool, queue, pixels,
           static_cast<uint32_t>(width), static_cast<uint32_t>(height),
           VK_FORMAT_R8G8B8A8_SRGB, mip, clamp, nearest);
    stbi_image_free(pixels);

    FJELL_GFX_DEBUG("Texture loaded from file: {}x{} nearest={}", width, height, nearest);
}

Texture::Texture(VkDevice device, VkPhysicalDevice physical_device,
                 VkCommandPool command_pool, VkQueue queue,
                 const uint8_t* pixels, uint32_t width, uint32_t height,
                 VkFormat format, uint32_t mip_levels, bool clamp_to_edge, bool nearest)
    : device_{device} {
    upload(physical_device, command_pool, queue, pixels, width, height, format, mip_levels, clamp_to_edge, nearest);

    FJELL_GFX_DEBUG("Texture loaded from memory: {}x{}", width, height);
}

Texture::~Texture() {
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, sampler_, nullptr);
    }
}

void Texture::upload(VkPhysicalDevice physical_device, VkCommandPool command_pool,
                     VkQueue queue, const void* pixels, uint32_t width, uint32_t height,
                     VkFormat format, uint32_t mip_levels_override, bool clamp_to_edge,
                     bool nearest) {
    VkDeviceSize image_size = static_cast<VkDeviceSize>(width) * height * 4;
    uint32_t mip_levels = mip_levels_override > 0 ? mip_levels_override :
        static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1;

    Buffer staging{device_, physical_device, image_size,
                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    staging.write(pixels, image_size);

    image_ = std::make_unique<Image>(
        device_, physical_device, width, height,
        format, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, mip_levels);

    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandPool = command_pool;
    alloc_info.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vk_check(vkAllocateCommandBuffers(device_, &alloc_info, &cmd),
             "Failed to allocate texture upload command buffer");

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(cmd, &begin_info),
             "Failed to begin texture upload command buffer");

    image_->transition_layout(cmd, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, mip_levels);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {width, height, 1};

    vkCmdCopyBufferToImage(cmd, staging.handle(), image_->handle(),
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    if (mip_levels > 1) {
        image_->generate_mipmaps(cmd, physical_device, width, height, mip_levels);
    } else {
        image_->transition_layout(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1);
    }

    vk_check(vkEndCommandBuffer(cmd),
             "Failed to end texture upload command buffer");

    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd;
    vk_check(vkQueueSubmit(queue, 1, &submit_info, VK_NULL_HANDLE),
             "Failed to submit texture upload command");
    vkQueueWaitIdle(queue);

    vkFreeCommandBuffers(device_, command_pool, 1, &cmd);

    image_->create_view(format, VK_IMAGE_ASPECT_COLOR_BIT, mip_levels);

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
