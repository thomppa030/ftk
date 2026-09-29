#include "renderer/resources/mesh.hpp"
#include "gpu/device.hpp"
#include "gpu/vulkan/native.hpp"

namespace fjell {

Mesh::Mesh(VmaAllocator allocator, gpu::Device& gpu,
           std::span<const Vertex> vertices,
           std::span<const uint32_t> indices)
    : vertex_buffer_{Buffer{allocator,
                     vertices.size_bytes(),
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                     VMA_MEMORY_USAGE_AUTO, 0, 0, gpu::vulkan::upload_families(gpu)}}
    , index_buffer_{Buffer{allocator,
                    indices.size_bytes(),
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                    VMA_MEMORY_USAGE_AUTO, 0, 0, gpu::vulkan::upload_families(gpu)}}
    , index_count_{static_cast<uint32_t>(indices.size())} {
    vertex_buffer_->share(gpu);
    index_buffer_->share(gpu);
    gpu.upload().to_buffer(vertex_buffer_->shared(), 0, vertices);
    gpu.upload().to_buffer(index_buffer_->shared(), 0, indices);
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
