#pragma once

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>

namespace fjell {

/// Copies a colour image to host memory as 8-bit sRGB RGBA. The source is
/// blitted into a transient image first, so it needs neither TRANSFER_SRC
/// usage nor an 8-bit format: HDR render targets and swapchain images read
/// back the same way. Blocks until the copy has finished and leaves the
/// source in the layout it was found in.
class ImageReadback {
public:
    struct Queue {
        VkDevice device{VK_NULL_HANDLE};
        VmaAllocator allocator{VK_NULL_HANDLE};
        VkCommandPool command_pool{VK_NULL_HANDLE};
        VkQueue queue{VK_NULL_HANDLE};
    };

    /// The whole image at its own size.
    ImageReadback(const Queue& queue, VkImage source, VkImageLayout source_layout,
                  uint32_t width, uint32_t height);

    /// A region of the image, scaled to `width` × `height` on the way. The
    /// scale is a single linear blit, so shrink by more than about half
    /// in a second CPU pass rather than here.
    ImageReadback(const Queue& queue, VkImage source, VkImageLayout source_layout,
                  VkRect2D region, uint32_t width, uint32_t height);
    ~ImageReadback();

    ImageReadback(const ImageReadback&) = delete;
    ImageReadback& operator=(const ImageReadback&) = delete;
    ImageReadback(ImageReadback&&) = delete;
    ImageReadback& operator=(ImageReadback&&) = delete;

    /// Row-major RGBA8, width * height * 4 bytes, valid for this object's
    /// lifetime.
    [[nodiscard]] const uint8_t* pixels() const { return static_cast<const uint8_t*>(mapped_); }
    [[nodiscard]] uint32_t width() const { return width_; }
    [[nodiscard]] uint32_t height() const { return height_; }

private:
    VmaAllocator allocator_{VK_NULL_HANDLE};
    VkBuffer buffer_{VK_NULL_HANDLE};
    VmaAllocation allocation_{VK_NULL_HANDLE};
    void* mapped_{nullptr};
    uint32_t width_{0};
    uint32_t height_{0};
};

} // namespace fjell
