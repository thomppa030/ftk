#pragma once

#include "renderer/image.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string>

namespace fjell {

class Texture {
public:
    Texture(VkDevice device, VkPhysicalDevice physical_device,
            VkCommandPool command_pool, VkQueue queue,
            const std::string& path);

    Texture(VkDevice device, VkPhysicalDevice physical_device,
            VkCommandPool command_pool, VkQueue queue,
            const uint8_t* pixels, uint32_t width, uint32_t height,
            VkFormat format = VK_FORMAT_R8G8B8A8_SRGB,
            uint32_t mip_levels = 0);

    ~Texture();

    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;
    Texture(Texture&&) = delete;
    Texture& operator=(Texture&&) = delete;

    [[nodiscard]] VkImageView image_view() const { return image_->view(); }
    [[nodiscard]] VkSampler sampler() const { return sampler_; }

private:
    void upload(VkPhysicalDevice physical_device, VkCommandPool command_pool,
                VkQueue queue, const void* pixels, uint32_t width, uint32_t height,
                VkFormat format = VK_FORMAT_R8G8B8A8_SRGB,
                uint32_t mip_levels = 0);

    VkDevice device_;
    std::unique_ptr<Image> image_;
    VkSampler sampler_{VK_NULL_HANDLE};
};

} // namespace fjell
