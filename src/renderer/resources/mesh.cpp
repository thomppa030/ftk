#include "renderer/resources/mesh.hpp"
#include "renderer/gpu/upload_context.hpp"

namespace fjell {

Mesh::Mesh(VmaAllocator allocator, UploadContext& upload,
           std::span<const Vertex> vertices,
           std::span<const uint32_t> indices)
    : vertex_buffer_{Buffer{allocator,
                     vertices.size_bytes(),
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                     VMA_MEMORY_USAGE_AUTO, 0, 0, upload.sharing_families()}}
    , index_buffer_{Buffer{allocator,
                    indices.size_bytes(),
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                    VMA_MEMORY_USAGE_AUTO, 0, 0, upload.sharing_families()}}
    , index_count_{static_cast<uint32_t>(indices.size())} {
    upload.upload_buffer(vertex_buffer_->handle(), 0, vertices.data(), vertices.size_bytes());
    upload.upload_buffer(index_buffer_->handle(), 0, indices.data(), indices.size_bytes());
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
