#include "renderer/resources/mesh.hpp"

namespace fjell {

Mesh::Mesh(VmaAllocator allocator, VkDevice device,
           VkCommandPool command_pool, VkQueue transfer_queue,
           std::span<const Vertex> vertices,
           std::span<const uint32_t> indices)
    : vertex_buffer_{Buffer{allocator,
                     vertices.size_bytes(),
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                     VMA_MEMORY_USAGE_AUTO}}
    , index_buffer_{Buffer{allocator,
                    indices.size_bytes(),
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                    VMA_MEMORY_USAGE_AUTO}}
    , index_count_{static_cast<uint32_t>(indices.size())} {
    // Stage and upload vertex data
    Buffer vertex_staging{allocator,
                          vertices.size_bytes(),
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VMA_MEMORY_USAGE_AUTO, VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT};
    vertex_staging.write(vertices.data(), vertices.size_bytes());
    Buffer::copy(device, command_pool, transfer_queue,
                 vertex_staging, *vertex_buffer_, vertices.size_bytes());

    // Stage and upload index data
    Buffer index_staging{allocator,
                         indices.size_bytes(),
                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VMA_MEMORY_USAGE_AUTO, VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT};
    index_staging.write(indices.data(), indices.size_bytes());
    Buffer::copy(device, command_pool, transfer_queue,
                 index_staging, *index_buffer_, indices.size_bytes());
}

Mesh::Mesh(MegaMeshEntry entry)
    : index_count_{entry.index_count}
    , mega_entry_{entry} {
}

void Mesh::bind(VkCommandBuffer cmd) const {
    if (!vertex_buffer_ || !index_buffer_) return;
    VkBuffer buffers[] = {vertex_buffer_->handle()};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, buffers, offsets);
    vkCmdBindIndexBuffer(cmd, index_buffer_->handle(), 0, VK_INDEX_TYPE_UINT32);
}

void Mesh::draw(VkCommandBuffer cmd) const {
    vkCmdDrawIndexed(cmd, index_count_, 1, 0, 0, 0);
}

} // namespace fjell
