#pragma once

#include "renderer/image.hpp"

#include <vulkan/vulkan.h>

#include <memory>
#include <string>

namespace fjell {

class Texture {
public:
    Texture(VkDevice device, VkPhysicalDevice physical_device,
            VkCommandPool command_pool, VkQueue queue,
            const std::string& path);
    ~Texture();

    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;
    Texture(Texture&&) = delete;
    Texture& operator=(Texture&&) = delete;

    [[nodiscard]] VkImageView image_view() const { return image_->view(); }
    [[nodiscard]] VkSampler sampler() const { return sampler_; }

private:
    VkDevice device_;
    std::unique_ptr<Image> image_;
    VkSampler sampler_{VK_NULL_HANDLE};
};

} // namespace fjell
