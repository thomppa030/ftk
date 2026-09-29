#include "renderer/resources/mega_buffer.hpp"
#include "gpu/render_encoder.hpp"
#include "gpu/device.hpp"
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
                       UploadContext& upload, gpu::Device& gpu)
    : gpu_{gpu}
    , vertex_buffer_{device, allocator, upload, gpu, MEGA_VERTEX_USAGE,
                     INITIAL_VERTEX_SIZE, MAX_VERTEX_SIZE, "mega_vertex"}
    , index_buffer_{device, allocator, upload, gpu, MEGA_INDEX_USAGE,
                    INITIAL_INDEX_SIZE, MAX_INDEX_SIZE, "mega_index"} {
    vertex_buffer_.share();
    index_buffer_.share();
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

    gpu_.upload().to_buffer(vertex_buffer_.shared(), vertex_byte_offset, vertices);
    gpu_.upload().to_buffer(index_buffer_.shared(), index_byte_offset, indices);

    MegaMeshEntry entry{};
    entry.vertex_offset = vertex_offset_;
    entry.vertex_count = static_cast<uint32_t>(vertices.size());
    entry.first_index = index_offset_;
    entry.index_count = static_cast<uint32_t>(indices.size());

    vertex_offset_ += static_cast<uint32_t>(vertices.size());
    index_offset_ += static_cast<uint32_t>(indices.size());

    return entry;
}

void MegaBuffer::update_vertices(const MegaMeshEntry& entry,
                                 const std::vector<Vertex>& vertices) {
    if (entry.vertex_count == 0 || vertices.size() != entry.vertex_count) return;
    const VkDeviceSize byte_offset =
        static_cast<VkDeviceSize>(entry.vertex_offset) * sizeof(Vertex);
    gpu_.upload().to_buffer(vertex_buffer_.shared(), byte_offset, vertices);
}

void MegaBuffer::bind(gpu::RenderEncoder& pass) const {
    pass.set_vertex_buffer(gpu::BufferRange(vertex_buffer_.shared()));
    pass.set_index_buffer(gpu::BufferRange(index_buffer_.shared()), gpu::IndexType::u32);
}

void MegaBuffer::bind(VkCommandBuffer cmd) const {
    VkBuffer buffers[] = {vertex_buffer_.handle()};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, buffers, offsets);
    vkCmdBindIndexBuffer(cmd, index_buffer_.handle(), 0, VK_INDEX_TYPE_UINT32);
}

} // namespace fjell
