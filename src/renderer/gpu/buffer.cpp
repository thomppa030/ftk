#include "renderer/gpu/buffer.hpp"
#include "gpu/vulkan/native.hpp"
#include "renderer/gpu/vk_check.hpp"

#include <cstring>
#include <stdexcept>
#include <utility>

namespace fjell {

Buffer::Buffer(VmaAllocator allocator, VkDeviceSize size,
               VkBufferUsageFlags usage, VmaMemoryUsage memory_usage,
               VmaAllocationCreateFlags flags, VkDeviceSize min_alignment,
               std::span<const uint32_t> sharing_families)
    : allocator_{allocator}, size_{size} {
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = usage;
    if (sharing_families.size() >= 2) {
        buffer_info.sharingMode = VK_SHARING_MODE_CONCURRENT;
        buffer_info.queueFamilyIndexCount = static_cast<uint32_t>(sharing_families.size());
        buffer_info.pQueueFamilyIndices = sharing_families.data();
    } else {
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    VmaAllocationCreateInfo alloc_ci{};
    alloc_ci.usage = memory_usage;
    alloc_ci.flags = flags;

    VkResult result;
    if (min_alignment > 0) {
        result = vmaCreateBufferWithAlignment(allocator_, &buffer_info, &alloc_ci,
                                              min_alignment, &buffer_, &allocation_, nullptr);
    } else {
        result = vmaCreateBuffer(allocator_, &buffer_info, &alloc_ci,
                                 &buffer_, &allocation_, nullptr);
    }
    if (result != VK_SUCCESS) {
        throw std::runtime_error("Failed to create buffer");
    }
}

Buffer::~Buffer() {
    if (mapped_) {
        unmap();
    }
    if (buffer_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator_, buffer_, allocation_);
    }
}

Buffer::Buffer(Buffer&& other) noexcept
    : allocator_{other.allocator_}
    , buffer_{std::exchange(other.buffer_, VK_NULL_HANDLE)}
    , allocation_{std::exchange(other.allocation_, VK_NULL_HANDLE)}
    , size_{other.size_}
    , mapped_{std::exchange(other.mapped_, nullptr)}
    , shared_{std::move(other.shared_)} {
}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        if (buffer_ != VK_NULL_HANDLE) {
            if (mapped_) unmap();
            vmaDestroyBuffer(allocator_, buffer_, allocation_);
        }

        allocator_ = other.allocator_;
        buffer_ = std::exchange(other.buffer_, VK_NULL_HANDLE);
        allocation_ = std::exchange(other.allocation_, VK_NULL_HANDLE);
        size_ = other.size_;
        mapped_ = std::exchange(other.mapped_, nullptr);
        shared_ = std::move(other.shared_);
    }
    return *this;
}

void Buffer::share(gpu::Device& device) {
    shared_ = gpu::vulkan::adopt(device, buffer_, size_);
}

void Buffer::map() {
    if (!mapped_) {
        vk_check(vmaMapMemory(allocator_, allocation_, &mapped_),
                 "Failed to map buffer memory");
    }
}

void Buffer::unmap() {
    if (mapped_) {
        vmaUnmapMemory(allocator_, allocation_);
        mapped_ = nullptr;
    }
}

void Buffer::write(const void* data, VkDeviceSize size) {
    if (!mapped_) {
        map();
    }
    std::memcpy(mapped_, data, size);
}

Result<Buffer> Buffer::create(VmaAllocator allocator, VkDeviceSize size,
                              VkBufferUsageFlags usage, VmaMemoryUsage memory_usage,
                              VmaAllocationCreateFlags flags) {
    Buffer buf{};
    buf.allocator_ = allocator;
    buf.size_ = size;

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo alloc_ci{};
    alloc_ci.usage = memory_usage;
    alloc_ci.flags = flags;

    if (vmaCreateBuffer(allocator, &buffer_info, &alloc_ci,
                        &buf.buffer_, &buf.allocation_, nullptr) != VK_SUCCESS) {
        return make_error("Failed to create buffer");
    }
    return buf;
}

} // namespace fjell
