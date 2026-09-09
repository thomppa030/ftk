#include "renderer/resources/mega_buffer.hpp"
#include "renderer/gpu/upload_context.hpp"
#include "core/log.hpp"

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

MegaBuffer::MegaBuffer(Device& device, VmaAllocator allocator,
                       UploadContext& upload, DeferredDeleter& deleter)
    : upload_{upload}
    , vertex_buffer_{device, allocator, upload, deleter, MEGA_VERTEX_USAGE,
                     INITIAL_VERTEX_SIZE, MAX_VERTEX_SIZE, "mega_vertex"}
    , index_buffer_{device, allocator, upload, deleter, MEGA_INDEX_USAGE,
                    INITIAL_INDEX_SIZE, MAX_INDEX_SIZE, "mega_index"} {
    FJELL_GFX_INFO("MegaBuffer created: {}MB vertex, {}MB index",
                   vertex_buffer_.capacity() / (1024 * 1024),
                   index_buffer_.capacity() / (1024 * 1024));
}

MegaMeshEntry MegaBuffer::upload(const std::vector<Vertex>& vertices,
                                  const std::vector<uint32_t>& indices) {
    VkDeviceSize vertex_size = vertices.size() * sizeof(Vertex);
    VkDeviceSize index_size = indices.size() * sizeof(uint32_t);

    VkDeviceSize vertex_byte_offset = static_cast<VkDeviceSize>(vertex_offset_) * sizeof(Vertex);
    VkDeviceSize index_byte_offset = static_cast<VkDeviceSize>(index_offset_) * sizeof(uint32_t);

    vertex_buffer_.ensure_capacity(vertex_byte_offset + vertex_size);
    index_buffer_.ensure_capacity(index_byte_offset + index_size);

    upload_.upload_buffer(vertex_buffer_.handle(), vertex_byte_offset,
                          vertices.data(), vertex_size);
    upload_.upload_buffer(index_buffer_.handle(), index_byte_offset,
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
    VkBuffer buffers[] = {vertex_buffer_.handle()};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, buffers, offsets);
    vkCmdBindIndexBuffer(cmd, index_buffer_.handle(), 0, VK_INDEX_TYPE_UINT32);
}

} // namespace fjell
