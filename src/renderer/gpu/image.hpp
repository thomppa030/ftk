#pragma once

#include "core/result.hpp"
#include "gpu/owned.hpp"
#include "gpu/texture.hpp"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>

namespace fjell {

class Image {
public:
    /// Throwing constructor — used by existing init-time code.
    Image(VmaAllocator allocator, uint32_t width, uint32_t height,
          VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage,
          VmaMemoryUsage memory_usage = VMA_MEMORY_USAGE_AUTO,
          uint32_t mip_levels = 1);

    /// Non-throwing factory — returns Result<Image> for error-propagating callers.
    [[nodiscard]] static Result<Image> create(
        VmaAllocator allocator, uint32_t width, uint32_t height,
        VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage,
        VmaMemoryUsage memory_usage = VMA_MEMORY_USAGE_AUTO,
        uint32_t mip_levels = 1);

    ~Image();

    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;
    Image(Image&&) noexcept;
    Image& operator=(Image&&) noexcept;

    [[nodiscard]] VkImage handle() const { return image_; }

    /// Hands the image to the GPU interface's device, which knows it by
    /// `shared()` from then on, for as long as this image lives: how an image
    /// made before the interface is written through `gpu::Upload`.
    void share(gpu::Device& device);

    /// The image as the GPU interface knows it; invalid until shared.
    [[nodiscard]] gpu::Texture shared() const { return shared_; }
    [[nodiscard]] VkImageView view() const { return view_; }
    [[nodiscard]] uint32_t width() const { return width_; }
    [[nodiscard]] uint32_t height() const { return height_; }

    void create_view(VkDevice device, VkFormat format, VkImageAspectFlags aspect,
                     uint32_t mip_levels = 1);

    /// Non-throwing create_view variant.
    [[nodiscard]] Result<> create_view_checked(VkDevice device, VkFormat format,
                                               VkImageAspectFlags aspect,
                                               uint32_t mip_levels = 1);

private:
    Image() = default;

    VmaAllocator allocator_{VK_NULL_HANDLE};
    VkDevice device_{VK_NULL_HANDLE};
    VkImage image_{VK_NULL_HANDLE};
    VmaAllocation allocation_{VK_NULL_HANDLE};
    VkImageView view_{VK_NULL_HANDLE};
    uint32_t width_{0};
    uint32_t height_{0};
    /// What the image was made as, which the interface is told when it is
    /// shared.
    VkImageCreateInfo made_as_{};
    gpu::Owned<gpu::Texture> shared_;
};

} // namespace fjell
