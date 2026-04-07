#pragma once

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

namespace fjell {

/// Lightweight helper to replace raw vkCreateImage + vkAllocateMemory + vkBindImageMemory
/// with VMA-backed allocation. Used by render passes, offscreen targets, and other code
/// that manages VkImage + VkDeviceMemory pairs directly (not via the Image class).

/// Create a VMA-backed image. Returns VK_SUCCESS on success.
/// Writes the image handle to *image and the allocation to *allocation.
/// Create a VMA-backed image. Returns VK_SUCCESS on success.
/// Writes the image handle to *image and the allocation to *allocation.
inline VkResult create_vma_image(VmaAllocator allocator,
                                 const VkImageCreateInfo* image_info,
                                 VkImage* image,
                                 VmaAllocation* allocation) {
    VmaAllocationCreateInfo alloc_ci{};
    alloc_ci.usage = VMA_MEMORY_USAGE_AUTO;
    // Dedicated allocation for images — driver zeroes dedicated pages,
    // preventing uninitialized VMA pool memory from causing first-frame artifacts.
    alloc_ci.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    return vmaCreateImage(allocator, image_info, &alloc_ci,
                          image, allocation, nullptr);
}

/// Destroy a VMA-backed image + free its allocation.
inline void destroy_vma_image(VmaAllocator allocator,
                              VkImage image,
                              VmaAllocation allocation) {
    if (image != VK_NULL_HANDLE) {
        vmaDestroyImage(allocator, image, allocation);
    }
}

/// Create a VMA-backed buffer (for staging buffers in icon_cache / content_browser).
/// Returns VK_SUCCESS on success.
inline VkResult create_vma_buffer(VmaAllocator allocator,
                                  const VkBufferCreateInfo* buffer_info,
                                  VmaAllocationCreateFlags flags,
                                  VkBuffer* buffer,
                                  VmaAllocation* allocation) {
    VmaAllocationCreateInfo alloc_ci{};
    alloc_ci.usage = VMA_MEMORY_USAGE_AUTO;
    alloc_ci.flags = flags;
    return vmaCreateBuffer(allocator, buffer_info, &alloc_ci,
                           buffer, allocation, nullptr);
}

/// Destroy a VMA-backed buffer + free its allocation.
inline void destroy_vma_buffer(VmaAllocator allocator,
                               VkBuffer buffer,
                               VmaAllocation allocation) {
    if (buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator, buffer, allocation);
    }
}

} // namespace fjell
