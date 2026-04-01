#include "renderer/gpu/buffer.hpp"
#include "renderer/gpu/vk_check.hpp"

#include <cstring>
#include <stdexcept>
#include <utility>

namespace fjell {

Buffer::Buffer(VkDevice device, VkPhysicalDevice physical_device,
               VkDeviceSize size, VkBufferUsageFlags usage,
               VkMemoryPropertyFlags properties)
    : device_{device}, size_{size} {
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device_, &buffer_info, nullptr, &buffer_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create buffer");
    }

    VkMemoryRequirements mem_reqs;
    vkGetBufferMemoryRequirements(device_, buffer_, &mem_reqs);

    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex = find_memory_type(
        physical_device, mem_reqs.memoryTypeBits, properties);

    if (vkAllocateMemory(device_, &alloc_info, nullptr, &memory_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate buffer memory");
    }

    vkBindBufferMemory(device_, buffer_, memory_, 0);
}

Buffer::~Buffer() {
    if (mapped_) {
        unmap();
    }
    if (buffer_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, buffer_, nullptr);
    }
    if (memory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, memory_, nullptr);
    }
}

Buffer::Buffer(Buffer&& other) noexcept
    : device_{other.device_}
    , buffer_{std::exchange(other.buffer_, VK_NULL_HANDLE)}
    , memory_{std::exchange(other.memory_, VK_NULL_HANDLE)}
    , size_{other.size_}
    , mapped_{std::exchange(other.mapped_, nullptr)} {
}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        if (buffer_ != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_, buffer_, nullptr);
        }
        if (memory_ != VK_NULL_HANDLE) {
            vkFreeMemory(device_, memory_, nullptr);
        }

        device_ = other.device_;
        buffer_ = std::exchange(other.buffer_, VK_NULL_HANDLE);
        memory_ = std::exchange(other.memory_, VK_NULL_HANDLE);
        size_ = other.size_;
        mapped_ = std::exchange(other.mapped_, nullptr);
    }
    return *this;
}

void Buffer::map() {
    if (!mapped_) {
        vk_check(vkMapMemory(device_, memory_, 0, size_, 0, &mapped_),
                 "Failed to map buffer memory");
    }
}

void Buffer::unmap() {
    if (mapped_) {
        vkUnmapMemory(device_, memory_);
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

uint32_t Buffer::find_memory_type(VkPhysicalDevice physical_device,
                                  uint32_t type_filter,
                                  VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_props);

    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((type_filter & (1 << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    throw std::runtime_error("Failed to find suitable memory type");
}

Result<Buffer> Buffer::create(VkDevice device, VkPhysicalDevice physical_device,
                              VkDeviceSize size, VkBufferUsageFlags usage,
                              VkMemoryPropertyFlags properties) {
    Buffer buf{};
    buf.device_ = device;
    buf.size_ = size;

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device, &buffer_info, nullptr, &buf.buffer_) != VK_SUCCESS) {
        return make_error("Failed to create buffer");
    }

    VkMemoryRequirements mem_reqs;
    vkGetBufferMemoryRequirements(device, buf.buffer_, &mem_reqs);

    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_props);

    uint32_t mem_type_index = UINT32_MAX;
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((mem_reqs.memoryTypeBits & (1 << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & properties) == properties) {
            mem_type_index = i;
            break;
        }
    }
    if (mem_type_index == UINT32_MAX) {
        vkDestroyBuffer(device, buf.buffer_, nullptr);
        buf.buffer_ = VK_NULL_HANDLE;
        return make_error("Failed to find suitable memory type for buffer");
    }

    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex = mem_type_index;

    if (vkAllocateMemory(device, &alloc_info, nullptr, &buf.memory_) != VK_SUCCESS) {
        vkDestroyBuffer(device, buf.buffer_, nullptr);
        buf.buffer_ = VK_NULL_HANDLE;
        return make_error("Failed to allocate buffer memory");
    }

    vkBindBufferMemory(device, buf.buffer_, buf.memory_, 0);
    return buf;
}

} // namespace fjell
