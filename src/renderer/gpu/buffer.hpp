#pragma once

#include "core/result.hpp"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <span>

namespace fjell {

class Buffer {
public:
    /// Throwing constructor — used by existing init-time code.
    /// `sharing_families` with two or more distinct entries creates the
    /// buffer VK_SHARING_MODE_CONCURRENT over those queue families, so its
    /// content stays defined when written on one queue and read on another
    /// without ownership transfers (e.g. transfer-queue uploads).
    Buffer(VmaAllocator allocator, VkDeviceSize size,
           VkBufferUsageFlags usage, VmaMemoryUsage memory_usage,
           VmaAllocationCreateFlags flags = 0,
           VkDeviceSize min_alignment = 0,
           std::span<const uint32_t> sharing_families = {});

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
    [[nodiscard]] VmaAllocation allocation() const { return allocation_; }

private:
    Buffer() = default;
    VmaAllocator allocator_{VK_NULL_HANDLE};
    VkBuffer buffer_{VK_NULL_HANDLE};
    VmaAllocation allocation_{VK_NULL_HANDLE};
    VkDeviceSize size_{0};
    void* mapped_{nullptr};
};

} // namespace fjell
