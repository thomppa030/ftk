#include "renderer/resources/texture.hpp"
#include "gpu/device.hpp"
#include "renderer/gpu/vk_check.hpp"
#include "core/log.hpp"

#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <stdexcept>

namespace fjell {

Texture::Texture(VkDevice device, VmaAllocator allocator,
                 VkPhysicalDevice physical_device, gpu::Device& gpu,
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
    upload(allocator, physical_device, gpu, pixels,
           static_cast<uint32_t>(width), static_cast<uint32_t>(height),
           VK_FORMAT_R8G8B8A8_SRGB, mip, clamp, nearest);
    stbi_image_free(pixels);

    FJELL_GFX_DEBUG("Texture loaded from file: {}x{} nearest={}", width, height, nearest);
}

Texture::Texture(VkDevice device, VmaAllocator allocator,
                 VkPhysicalDevice physical_device, gpu::Device& gpu,
                 const uint8_t* pixels, uint32_t width, uint32_t height,
                 VkFormat format, uint32_t mip_levels, bool clamp_to_edge, bool nearest)
    : device_{device} {
    upload(allocator, physical_device, gpu, pixels, width, height, format, mip_levels, clamp_to_edge, nearest);

    FJELL_GFX_DEBUG("Texture loaded from memory: {}x{}", width, height);
}

Texture::~Texture() {
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, sampler_, nullptr);
    }
}

namespace {

// Bytes one texel of `format` takes in the pixels handed to upload(): four
// 8-bit channels for the images and atlases, four 16-bit ones for a LUT strip.
VkDeviceSize texel_bytes(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_R8G8B8A8_UNORM:
            return 4;
        case VK_FORMAT_R16G16B16A16_UNORM:
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            return 8;
        case VK_FORMAT_R32G32B32A32_SFLOAT:
            return 16;
        default:
            throw std::runtime_error("Texture: no texel size known for this format");
    }
}

} // namespace

void Texture::upload(VmaAllocator allocator, VkPhysicalDevice physical_device,
                     gpu::Device& gpu,
                     const void* pixels, uint32_t width, uint32_t height,
                     VkFormat format, uint32_t mip_levels_override, bool clamp_to_edge,
                     bool nearest) {
    VkDeviceSize image_size = static_cast<VkDeviceSize>(width) * height * texel_bytes(format);
    uint32_t mip_levels = mip_levels_override > 0 ? mip_levels_override :
        static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1;

    image_ = std::make_unique<Image>(
        allocator, width, height,
        format, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT,
        VMA_MEMORY_USAGE_AUTO, mip_levels);
    image_->create_view(device_, format, VK_IMAGE_ASPECT_COLOR_BIT, mip_levels);
    image_->share(gpu);

    // Materials sample it in fragment shaders, and the GI passes trace
    // through the same bindless array from compute.
    gpu.upload().to_texture(
        gpu::mip(image_->shared(), 0), {},
        std::span(static_cast<const std::byte*>(pixels), image_size),
        {.after = gpu::Access::sampled_fragment | gpu::Access::sampled_compute,
         .generate_mips = mip_levels > 1});

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
