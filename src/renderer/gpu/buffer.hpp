#pragma once

#include "core/result.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>

namespace fjell {

class Buffer {
public:
    /// Throwing constructor — used by existing init-time code.
    Buffer(VkDevice device, VkPhysicalDevice physical_device,
           VkDeviceSize size, VkBufferUsageFlags usage,
           VkMemoryPropertyFlags properties);

    /// Non-throwing factory — returns Result<Buffer> for error-propagating callers.
    [[nodiscard]] static Result<Buffer> create(
        VkDevice device, VkPhysicalDevice physical_device,
        VkDeviceSize size, VkBufferUsageFlags usage,
        VkMemoryPropertyFlags properties);

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

    [[nodiscard]] static uint32_t find_memory_type(
        VkPhysicalDevice physical_device, uint32_t type_filter,
        VkMemoryPropertyFlags properties);

private:
    Buffer() = default;
    VkDevice device_{VK_NULL_HANDLE};
    VkBuffer buffer_{VK_NULL_HANDLE};
    VkDeviceMemory memory_{VK_NULL_HANDLE};
    VkDeviceSize size_{0};
    void* mapped_{nullptr};
};

} // namespace fjell
