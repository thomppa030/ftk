#include "renderer/mesh.hpp"

namespace fjell {

Mesh::Mesh(VkDevice device, VkPhysicalDevice physical_device,
           VkCommandPool command_pool, VkQueue transfer_queue,
           std::span<const Vertex> vertices,
           std::span<const uint32_t> indices)
    : vertex_buffer_{device, physical_device,
                     vertices.size_bytes(),
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT}
    , index_buffer_{device, physical_device,
                    indices.size_bytes(),
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT}
    , index_count_{static_cast<uint32_t>(indices.size())} {
    // Stage and upload vertex data
    Buffer vertex_staging{device, physical_device,
                          vertices.size_bytes(),
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    vertex_staging.write(vertices.data(), vertices.size_bytes());
    Buffer::copy(device, command_pool, transfer_queue,
                 vertex_staging, vertex_buffer_, vertices.size_bytes());

    // Stage and upload index data
    Buffer index_staging{device, physical_device,
                         indices.size_bytes(),
                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    index_staging.write(indices.data(), indices.size_bytes());
    Buffer::copy(device, command_pool, transfer_queue,
                 index_staging, index_buffer_, indices.size_bytes());
}

void Mesh::bind(VkCommandBuffer cmd) const {
    VkBuffer buffers[] = {vertex_buffer_.handle()};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, buffers, offsets);
    vkCmdBindIndexBuffer(cmd, index_buffer_.handle(), 0, VK_INDEX_TYPE_UINT32);
}

void Mesh::draw(VkCommandBuffer cmd) const {
    vkCmdDrawIndexed(cmd, index_count_, 1, 0, 0, 0);
}

} // namespace fjell
