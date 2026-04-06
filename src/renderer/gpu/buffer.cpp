#include "renderer/gpu/buffer.hpp"
#include "renderer/gpu/vk_check.hpp"

#include <cstring>
#include <stdexcept>
#include <utility>

namespace fjell {

Buffer::Buffer(VmaAllocator allocator, VkDeviceSize size,
               VkBufferUsageFlags usage, VmaMemoryUsage memory_usage,
               VmaAllocationCreateFlags flags)
    : allocator_{allocator}, size_{size} {
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo alloc_ci{};
    alloc_ci.usage = memory_usage;
    alloc_ci.flags = flags;

    if (vmaCreateBuffer(allocator_, &buffer_info, &alloc_ci,
                        &buffer_, &allocation_, nullptr) != VK_SUCCESS) {
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
    , mapped_{std::exchange(other.mapped_, nullptr)} {
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
    }
    return *this;
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

void Buffer::copy(VkDevice device, VkCommandPool command_pool,
                  VkQueue queue, const Buffer& src, Buffer& dst,
                  VkDeviceSize size) {
    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandPool = command_pool;
    alloc_info.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vk_check(vkAllocateCommandBuffers(device, &alloc_info, &cmd),
             "Failed to allocate copy command buffer");

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vk_check(vkBeginCommandBuffer(cmd, &begin_info),
             "Failed to begin copy command buffer");

    VkBufferCopy copy_region{};
    copy_region.size = size;
    vkCmdCopyBuffer(cmd, src.handle(), dst.handle(), 1, &copy_region);

    vk_check(vkEndCommandBuffer(cmd),
             "Failed to end copy command buffer");

    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd;

    vk_check(vkQueueSubmit(queue, 1, &submit_info, VK_NULL_HANDLE),
             "Failed to submit copy command");
    vkQueueWaitIdle(queue);

    vkFreeCommandBuffers(device, command_pool, 1, &cmd);
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
