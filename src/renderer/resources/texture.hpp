#pragma once

#include "renderer/gpu/image.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string>

namespace fjell {

class UploadContext;

class Texture {
public:
    Texture(VkDevice device, VmaAllocator allocator,
            VkPhysicalDevice physical_device, UploadContext& upload,
            const std::string& path, bool nearest = false);

    Texture(VkDevice device, VmaAllocator allocator,
            VkPhysicalDevice physical_device, UploadContext& upload,
            const uint8_t* pixels, uint32_t width, uint32_t height,
            VkFormat format = VK_FORMAT_R8G8B8A8_SRGB,
            uint32_t mip_levels = 0,
            bool clamp_to_edge = false,
            bool nearest = false);

    ~Texture();

    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;
    Texture(Texture&&) = delete;
    Texture& operator=(Texture&&) = delete;

    [[nodiscard]] VkImageView image_view() const { return image_->view(); }
    [[nodiscard]] VkSampler sampler() const { return sampler_; }
    [[nodiscard]] uint32_t width() const { return image_->width(); }
    [[nodiscard]] uint32_t height() const { return image_->height(); }

private:
    void upload(VmaAllocator allocator, VkPhysicalDevice physical_device,
                UploadContext& upload_ctx,
                const void* pixels, uint32_t width, uint32_t height,
                VkFormat format = VK_FORMAT_R8G8B8A8_SRGB,
                uint32_t mip_levels = 0,
                bool clamp_to_edge = false,
                bool nearest = false);

    VkDevice device_;
    std::unique_ptr<Image> image_;
    VkSampler sampler_{VK_NULL_HANDLE};
};

} // namespace fjell
