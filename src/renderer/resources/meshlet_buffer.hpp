#pragma once

#include "renderer/gpu/growable_buffer.hpp"
#include "renderer/resources/meshlet.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>

namespace fjell {

class Device;
class UploadContext;
namespace gpu {
class Device;
}

struct MeshletRange {
    uint32_t meshlet_offset{0};  // first meshlet index in the global array
    uint32_t meshlet_count{0};
    uint32_t vertex_offset{0};   // mega buffer vertex offset (for vertex fetch)
};

class MeshletBuffer {
public:
    MeshletBuffer(Device& device, VmaAllocator allocator,
                  UploadContext& upload, gpu::Device& gpu);

    MeshletRange upload(const MeshletBuildResult& result, uint32_t mega_vertex_offset);

    [[nodiscard]] VkBuffer meshlet_buffer() const { return meshlet_buffer_.handle(); }
    [[nodiscard]] VkBuffer bounds_buffer() const { return bounds_buffer_.handle(); }
    [[nodiscard]] VkBuffer vertex_index_buffer() const { return vertex_index_buffer_.handle(); }
    [[nodiscard]] VkBuffer triangle_buffer() const { return triangle_buffer_.handle(); }

    [[nodiscard]] VkDeviceSize meshlet_buffer_size() const { return meshlet_used_; }
    [[nodiscard]] VkDeviceSize bounds_buffer_size() const { return bounds_used_; }
    [[nodiscard]] VkDeviceSize vertex_index_buffer_size() const { return vertex_index_used_; }
    [[nodiscard]] VkDeviceSize triangle_buffer_size() const { return triangle_used_; }

    [[nodiscard]] uint32_t total_meshlets() const { return meshlet_count_; }

private:
    UploadContext& upload_;

    GrowableBuffer meshlet_buffer_;
    GrowableBuffer bounds_buffer_;
    GrowableBuffer vertex_index_buffer_;
    GrowableBuffer triangle_buffer_;

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
    // Virtual reservations (sparse mode commits memory page-by-page).
    static constexpr VkDeviceSize MAX_MESHLET_SIZE = 128ULL * 1024 * 1024;
    static constexpr VkDeviceSize MAX_BOUNDS_SIZE = 256ULL * 1024 * 1024;
    static constexpr VkDeviceSize MAX_VERTEX_INDEX_SIZE = 512ULL * 1024 * 1024;
    static constexpr VkDeviceSize MAX_TRIANGLE_SIZE = 256ULL * 1024 * 1024;
};

} // namespace fjell
