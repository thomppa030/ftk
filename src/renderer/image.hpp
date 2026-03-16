#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

namespace fjell {

class Image {
public:
    Image(VkDevice device, VkPhysicalDevice physical_device,
          uint32_t width, uint32_t height, VkFormat format,
          VkImageTiling tiling, VkImageUsageFlags usage,
          VkMemoryPropertyFlags properties, uint32_t mip_levels = 1);
    ~Image();

    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;
    Image(Image&&) = delete;
    Image& operator=(Image&&) = delete;

    void transition_layout(VkCommandBuffer cmd,
                           VkImageLayout old_layout, VkImageLayout new_layout,
                           uint32_t mip_levels = 1);

    void generate_mipmaps(VkCommandBuffer cmd, VkPhysicalDevice physical_device,
                          uint32_t width, uint32_t height, uint32_t mip_levels);

    [[nodiscard]] VkImage handle() const { return image_; }
    [[nodiscard]] VkImageView view() const { return view_; }

    void create_view(VkFormat format, VkImageAspectFlags aspect, uint32_t mip_levels = 1);

private:
    VkDevice device_;
    VkImage image_{VK_NULL_HANDLE};
    VkDeviceMemory memory_{VK_NULL_HANDLE};
    VkImageView view_{VK_NULL_HANDLE};
};

} // namespace fjell
