#pragma once

#include "core/result.hpp"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>

namespace fjell {

class Buffer {
public:
    /// Throwing constructor — used by existing init-time code.
    Buffer(VmaAllocator allocator, VkDeviceSize size,
           VkBufferUsageFlags usage, VmaMemoryUsage memory_usage,
           VmaAllocationCreateFlags flags = 0,
           VkDeviceSize min_alignment = 0);

    /// Non-throwing factory — returns Result<Buffer> for error-propagating callers.
    [[nodiscard]] static Result<Buffer> create(
        VmaAllocator allocator, VkDeviceSize size,
        VkBufferUsageFlags usage, VmaMemoryUsage memory_usage,
        VmaAllocationCreateFlags flags = 0);

    ~Buffer();

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    Buffer(Buffer&&) noexcept;
    Buffer& operator=(Buffer&&) noexcept;

    void map();
    void unmap();
    void write(const void* data, VkDeviceSize size);

    [[nodiscard]] VkBuffer handle() const { return buffer_; }
    [[nodiscard]] VkDeviceSize size() const { return size_; }
    [[nodiscard]] void* mapped() const { return mapped_; }

    static void copy(VkDevice device, VkCommandPool command_pool,
                     VkQueue queue, const Buffer& src, Buffer& dst,
                     VkDeviceSize size);

private:
    Buffer() = default;
    VmaAllocator allocator_{VK_NULL_HANDLE};
    VkBuffer buffer_{VK_NULL_HANDLE};
    VmaAllocation allocation_{VK_NULL_HANDLE};
    VkDeviceSize size_{0};
    void* mapped_{nullptr};
};

} // namespace fjell
