#include "renderer/resources/mega_buffer.hpp"
#include "core/log.hpp"

#include <stdexcept>

namespace fjell {

MegaBuffer::MegaBuffer(VkDevice device, VmaAllocator allocator,
                       VkCommandPool command_pool, VkQueue queue)
    : device_{device}
    , allocator_{allocator}
    , command_pool_{command_pool}
    , queue_{queue}
    , vertex_capacity_{INITIAL_VERTEX_SIZE}
    , index_capacity_{INITIAL_INDEX_SIZE} {
    vertex_buffer_ = std::make_unique<Buffer>(
        allocator_, vertex_capacity_,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO);

    index_buffer_ = std::make_unique<Buffer>(
        allocator_, index_capacity_,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO);

    FJELL_GFX_INFO("MegaBuffer created: {}MB vertex, {}MB index",
                   vertex_capacity_ / (1024 * 1024),
                   index_capacity_ / (1024 * 1024));
}

MegaMeshEntry MegaBuffer::upload(const std::vector<Vertex>& vertices,
                                  const std::vector<uint32_t>& indices) {
    VkDeviceSize vertex_size = vertices.size() * sizeof(Vertex);
    VkDeviceSize index_size = indices.size() * sizeof(uint32_t);

    VkDeviceSize vertex_byte_offset = static_cast<VkDeviceSize>(vertex_offset_) * sizeof(Vertex);
    VkDeviceSize index_byte_offset = static_cast<VkDeviceSize>(index_offset_) * sizeof(uint32_t);

    // Grow if needed
    if (vertex_byte_offset + vertex_size > vertex_capacity_) {
        grow_vertex_buffer((vertex_byte_offset + vertex_size) * 2);
    }
    if (index_byte_offset + index_size > index_capacity_) {
        grow_index_buffer((index_byte_offset + index_size) * 2);
    }

    // Stage and copy vertex data
    Buffer vertex_staging{allocator_, vertex_size,
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VMA_MEMORY_USAGE_AUTO,
                          VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT};
    vertex_staging.write(vertices.data(), vertex_size);

    // Stage and copy index data
    Buffer index_staging{allocator_, index_size,
                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VMA_MEMORY_USAGE_AUTO,
                         VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT};
    index_staging.write(indices.data(), index_size);

    // Submit both copies in one command buffer
    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandPool = command_pool_;
    alloc_info.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device_, &alloc_info, &cmd);

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin_info);

    VkBufferCopy vertex_copy{};
    vertex_copy.srcOffset = 0;
    vertex_copy.dstOffset = vertex_byte_offset;
    vertex_copy.size = vertex_size;
    vkCmdCopyBuffer(cmd, vertex_staging.handle(), vertex_buffer_->handle(), 1, &vertex_copy);

    VkBufferCopy index_copy{};
    index_copy.srcOffset = 0;
    index_copy.dstOffset = index_byte_offset;
    index_copy.size = index_size;
    vkCmdCopyBuffer(cmd, index_staging.handle(), index_buffer_->handle(), 1, &index_copy);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd;
    vkQueueSubmit(queue_, 1, &submit_info, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_);

    vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);

    MegaMeshEntry entry{};
    entry.vertex_offset = vertex_offset_;
    entry.first_index = index_offset_;
    entry.index_count = static_cast<uint32_t>(indices.size());

    vertex_offset_ += static_cast<uint32_t>(vertices.size());
    index_offset_ += static_cast<uint32_t>(indices.size());

    return entry;
}

void MegaBuffer::bind(VkCommandBuffer cmd) const {
    VkBuffer buffers[] = {vertex_buffer_->handle()};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, buffers, offsets);
    vkCmdBindIndexBuffer(cmd, index_buffer_->handle(), 0, VK_INDEX_TYPE_UINT32);
}

void MegaBuffer::grow_vertex_buffer(VkDeviceSize new_size) {
    FJELL_GFX_INFO("MegaBuffer: growing vertex buffer to {}MB", new_size / (1024 * 1024));

    auto new_buffer = std::make_unique<Buffer>(
        allocator_, new_size,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO);

    if (vertex_offset_ > 0) {
        VkDeviceSize used = static_cast<VkDeviceSize>(vertex_offset_) * sizeof(Vertex);
        Buffer::copy(device_, command_pool_, queue_, *vertex_buffer_, *new_buffer, used);
    }

    vertex_buffer_ = std::move(new_buffer);
    vertex_capacity_ = new_size;
}

void MegaBuffer::grow_index_buffer(VkDeviceSize new_size) {
    FJELL_GFX_INFO("MegaBuffer: growing index buffer to {}MB", new_size / (1024 * 1024));

    auto new_buffer = std::make_unique<Buffer>(
        allocator_, new_size,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO);

    if (index_offset_ > 0) {
        VkDeviceSize used = static_cast<VkDeviceSize>(index_offset_) * sizeof(uint32_t);
        Buffer::copy(device_, command_pool_, queue_, *index_buffer_, *new_buffer, used);
    }

    index_buffer_ = std::move(new_buffer);
    index_capacity_ = new_size;
}

} // namespace fjell
