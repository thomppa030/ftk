#include "renderer/resources/mega_buffer.hpp"
#include "renderer/gpu/deferred_deleter.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/gpu/upload_context.hpp"
#include "core/log.hpp"

#include <stdexcept>

namespace fjell {

namespace {

constexpr VkBufferUsageFlags MEGA_VERTEX_USAGE =
    VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

constexpr VkBufferUsageFlags MEGA_INDEX_USAGE =
    VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
    VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

} // namespace

MegaBuffer::MegaBuffer(VmaAllocator allocator, UploadContext& upload,
                       DeferredDeleter& deleter)
    : allocator_{allocator}
    , upload_{upload}
    , deleter_{deleter}
    , vertex_capacity_{INITIAL_VERTEX_SIZE}
    , index_capacity_{INITIAL_INDEX_SIZE} {
    vertex_buffer_ = std::make_unique<Buffer>(
        allocator_, vertex_capacity_, MEGA_VERTEX_USAGE,
        VMA_MEMORY_USAGE_AUTO, 0, 0, upload_.sharing_families());

    index_buffer_ = std::make_unique<Buffer>(
        allocator_, index_capacity_, MEGA_INDEX_USAGE,
        VMA_MEMORY_USAGE_AUTO, 0, 0, upload_.sharing_families());

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

    upload_.upload_buffer(vertex_buffer_->handle(), vertex_byte_offset,
                          vertices.data(), vertex_size);
    upload_.upload_buffer(index_buffer_->handle(), index_byte_offset,
                          indices.data(), index_size);

    MegaMeshEntry entry{};
    entry.vertex_offset = vertex_offset_;
    entry.vertex_count = static_cast<uint32_t>(vertices.size());
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
        allocator_, new_size, MEGA_VERTEX_USAGE,
        VMA_MEMORY_USAGE_AUTO, 0, 0, upload_.sharing_families());

    if (vertex_offset_ > 0) {
        VkDeviceSize used = static_cast<VkDeviceSize>(vertex_offset_) * sizeof(Vertex);
        upload_.copy_buffer(vertex_buffer_->handle(), new_buffer->handle(), used);
    }

    // In-flight frames still read the old buffer — retire, don't destroy.
    deleter_.retire(std::move(vertex_buffer_));
    vertex_buffer_ = std::move(new_buffer);
    vertex_capacity_ = new_size;
    ++generation_;  // new VkBuffer = new device address; baked BLAS addrs now stale
}

void MegaBuffer::grow_index_buffer(VkDeviceSize new_size) {
    FJELL_GFX_INFO("MegaBuffer: growing index buffer to {}MB", new_size / (1024 * 1024));

    auto new_buffer = std::make_unique<Buffer>(
        allocator_, new_size, MEGA_INDEX_USAGE,
        VMA_MEMORY_USAGE_AUTO, 0, 0, upload_.sharing_families());

    if (index_offset_ > 0) {
        VkDeviceSize used = static_cast<VkDeviceSize>(index_offset_) * sizeof(uint32_t);
        upload_.copy_buffer(index_buffer_->handle(), new_buffer->handle(), used);
    }

    deleter_.retire(std::move(index_buffer_));
    index_buffer_ = std::move(new_buffer);
    index_capacity_ = new_size;
    ++generation_;  // new VkBuffer = new device address; baked BLAS addrs now stale
}

} // namespace fjell
