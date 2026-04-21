#pragma once

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <string>

namespace fjell {

/// Attach a human-readable name to a Vulkan image via VK_EXT_debug_utils.
/// Validation messages and RenderDoc captures display this name instead
/// of the raw handle. Silent no-op when debug utils aren't available
/// (release builds without the extension loaded).
inline void set_image_debug_name(VkDevice device, VkImage image, const std::string& name) {
    if (image == VK_NULL_HANDLE) { return; }
    static auto fn = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
        vkGetDeviceProcAddr(device, "vkSetDebugUtilsObjectNameEXT"));
    if (fn == nullptr) { return; }
    VkDebugUtilsObjectNameInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    info.objectType = VK_OBJECT_TYPE_IMAGE;
    info.objectHandle = reinterpret_cast<uint64_t>(image);
    info.pObjectName = name.c_str();
    fn(device, &info);
}

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
