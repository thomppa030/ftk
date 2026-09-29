#pragma once

#include "renderer/gpu/growable_buffer.hpp"
#include "renderer/resources/vertex.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace fjell {

class Device;
class UploadContext;
namespace gpu {
class Device;
}

struct MegaMeshEntry {
    uint32_t vertex_offset{0};
    uint32_t vertex_count{0};
    uint32_t first_index{0};
    uint32_t index_count{0};
    uint32_t meshlet_offset{0};
    uint32_t meshlet_count{0};
};

class MegaBuffer {
public:
    MegaBuffer(Device& device, VmaAllocator allocator,
               UploadContext& upload, gpu::Device& gpu);

    MegaMeshEntry upload(const std::vector<Vertex>& vertices,
                         const std::vector<uint32_t>& indices);

    /// Overwrite the vertices of a mesh already uploaded, leaving its indices
    /// and its place in the buffer alone. For geometry whose shape changes but
    /// whose topology does not — a sculpted terrain, whose ray-tracing mesh is
    /// the same grid at new heights. The vertex count must match what was
    /// uploaded; a different count needs a new entry.
    ///
    /// A consumer that baked device addresses from the entry (a BLAS) is not
    /// invalidated by this, since nothing moves, but it is now describing
    /// different triangles and must be rebuilt to match.
    void update_vertices(const MegaMeshEntry& entry, const std::vector<Vertex>& vertices);

    void bind(VkCommandBuffer cmd) const;

    [[nodiscard]] uint32_t vertex_count() const { return vertex_offset_; }
    [[nodiscard]] uint32_t index_count() const { return index_offset_; }
    [[nodiscard]] VkBuffer vertex_buffer_handle() const { return vertex_buffer_.handle(); }
    [[nodiscard]] VkDeviceSize vertex_buffer_size() const {
        return static_cast<VkDeviceSize>(vertex_offset_) * sizeof(Vertex);
    }
    [[nodiscard]] VkBuffer index_buffer_handle() const { return index_buffer_.handle(); }
    [[nodiscard]] VkDeviceSize index_buffer_size() const {
        return static_cast<VkDeviceSize>(index_offset_) * sizeof(uint32_t);
    }

    /// Increments every time the vertex or index buffer is reallocated.
    /// A reallocation gives the buffer a NEW device address, which invalidates
    /// any consumer that baked the old address — notably ray-tracing BLAS
    /// geometry. Such consumers stamp this value and rebuild when it changes.
    /// With sparse binding the buffers grow in place and this never moves.
    [[nodiscard]] uint32_t generation() const {
        return vertex_buffer_.generation() + index_buffer_.generation();
    }

private:
    gpu::Device& gpu_;

    GrowableBuffer vertex_buffer_;
    GrowableBuffer index_buffer_;

    uint32_t vertex_offset_{0};
    uint32_t index_offset_{0};

    static constexpr VkDeviceSize INITIAL_VERTEX_SIZE = 16 * 1024 * 1024;
    static constexpr VkDeviceSize INITIAL_INDEX_SIZE = 8 * 1024 * 1024;
    // Virtual reservations (sparse mode commits memory page-by-page).
    static constexpr VkDeviceSize MAX_VERTEX_SIZE = 1024ULL * 1024 * 1024;
    static constexpr VkDeviceSize MAX_INDEX_SIZE = 512ULL * 1024 * 1024;
};

} // namespace fjell
