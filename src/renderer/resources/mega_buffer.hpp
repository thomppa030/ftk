#pragma once

#include "renderer/gpu/buffer.hpp"
#include "renderer/resources/vertex.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace fjell {

struct MegaMeshEntry {
    uint32_t vertex_offset{0};
    uint32_t first_index{0};
    uint32_t index_count{0};
    uint32_t meshlet_offset{0};
    uint32_t meshlet_count{0};
};

class MegaBuffer {
public:
    MegaBuffer(VkDevice device, VmaAllocator allocator,
               VkCommandPool command_pool, VkQueue queue);

    MegaMeshEntry upload(const std::vector<Vertex>& vertices,
                         const std::vector<uint32_t>& indices);

    void bind(VkCommandBuffer cmd) const;

    [[nodiscard]] uint32_t vertex_count() const { return vertex_offset_; }
    [[nodiscard]] uint32_t index_count() const { return index_offset_; }
    [[nodiscard]] VkBuffer vertex_buffer_handle() const { return vertex_buffer_->handle(); }
    [[nodiscard]] VkDeviceSize vertex_buffer_size() const {
        return static_cast<VkDeviceSize>(vertex_offset_) * sizeof(Vertex);
    }
    [[nodiscard]] VkBuffer index_buffer_handle() const { return index_buffer_->handle(); }
    [[nodiscard]] VkDeviceSize index_buffer_size() const {
        return static_cast<VkDeviceSize>(index_offset_) * sizeof(uint32_t);
    }

private:
    void grow_vertex_buffer(VkDeviceSize new_size);
    void grow_index_buffer(VkDeviceSize new_size);

    VkDevice device_;
    VmaAllocator allocator_;
    VkCommandPool command_pool_;
    VkQueue queue_;

    std::unique_ptr<Buffer> vertex_buffer_;
    std::unique_ptr<Buffer> index_buffer_;

    uint32_t vertex_offset_{0};
    uint32_t index_offset_{0};

    VkDeviceSize vertex_capacity_;
    VkDeviceSize index_capacity_;

    static constexpr VkDeviceSize INITIAL_VERTEX_SIZE = 16 * 1024 * 1024;
    static constexpr VkDeviceSize INITIAL_INDEX_SIZE = 8 * 1024 * 1024;
};

} // namespace fjell
