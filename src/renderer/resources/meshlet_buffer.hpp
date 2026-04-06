#pragma once

#include "renderer/gpu/buffer.hpp"
#include "renderer/resources/meshlet.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>

namespace fjell {

struct MeshletRange {
    uint32_t meshlet_offset{0};  // first meshlet index in the global array
    uint32_t meshlet_count{0};
    uint32_t vertex_offset{0};   // mega buffer vertex offset (for vertex fetch)
};

class MeshletBuffer {
public:
    MeshletBuffer(VkDevice device, VmaAllocator allocator,
                  VkCommandPool command_pool, VkQueue queue);

    MeshletRange upload(const MeshletBuildResult& result, uint32_t mega_vertex_offset);

    [[nodiscard]] VkBuffer meshlet_buffer() const { return meshlet_buffer_->handle(); }
    [[nodiscard]] VkBuffer bounds_buffer() const { return bounds_buffer_->handle(); }
    [[nodiscard]] VkBuffer vertex_index_buffer() const { return vertex_index_buffer_->handle(); }
    [[nodiscard]] VkBuffer triangle_buffer() const { return triangle_buffer_->handle(); }

    [[nodiscard]] VkDeviceSize meshlet_buffer_size() const { return meshlet_used_; }
    [[nodiscard]] VkDeviceSize bounds_buffer_size() const { return bounds_used_; }
    [[nodiscard]] VkDeviceSize vertex_index_buffer_size() const { return vertex_index_used_; }
    [[nodiscard]] VkDeviceSize triangle_buffer_size() const { return triangle_used_; }

    [[nodiscard]] uint32_t total_meshlets() const { return meshlet_count_; }

private:
    void grow_buffer(std::unique_ptr<Buffer>& buf, VkDeviceSize new_size,
                     VkDeviceSize used, VkBufferUsageFlags usage);

    VkDevice device_;
    VmaAllocator allocator_;
    VkCommandPool command_pool_;
    VkQueue queue_;

    std::unique_ptr<Buffer> meshlet_buffer_;
    std::unique_ptr<Buffer> bounds_buffer_;
    std::unique_ptr<Buffer> vertex_index_buffer_;
    std::unique_ptr<Buffer> triangle_buffer_;

    VkDeviceSize meshlet_capacity_;
    VkDeviceSize bounds_capacity_;
    VkDeviceSize vertex_index_capacity_;
    VkDeviceSize triangle_capacity_;

    VkDeviceSize meshlet_used_{0};
    VkDeviceSize bounds_used_{0};
    VkDeviceSize vertex_index_used_{0};
    VkDeviceSize triangle_used_{0};          // padded bytes used in GPU buffer
    VkDeviceSize triangle_raw_bytes_{0};     // unpadded raw bytes (for offset calculation)

    uint32_t meshlet_count_{0};

    static constexpr VkDeviceSize INITIAL_MESHLET_SIZE = 256 * 1024;     // ~16K meshlets
    static constexpr VkDeviceSize INITIAL_BOUNDS_SIZE = 768 * 1024;       // ~16K bounds
    static constexpr VkDeviceSize INITIAL_VERTEX_INDEX_SIZE = 1024 * 1024; // ~256K indices
    static constexpr VkDeviceSize INITIAL_TRIANGLE_SIZE = 512 * 1024;     // ~170K triangles
};

} // namespace fjell
