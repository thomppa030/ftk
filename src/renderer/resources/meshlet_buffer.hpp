#pragma once

#include "renderer/resources/meshlet.hpp"

#include "gpu/buffer.hpp"
#include "gpu/owned.hpp"

#include <cstdint>

namespace fjell {

namespace gpu {
class Device;
}

struct MeshletRange {
    uint32_t meshlet_offset{0};  // first meshlet index in the global array
    uint32_t meshlet_count{0};
    uint32_t vertex_offset{0};   // mega buffer vertex offset (for vertex fetch)
};

/// Every static mesh's meshlets: their records, bounds, vertex indices and
/// packed triangles, one mesh after another. All four grow as meshes are
/// added, keeping their handles.
class MeshletBuffer {
public:
    /// Throws when the buffers cannot be made.
    explicit MeshletBuffer(gpu::Device& gpu);

    /// Appends a mesh's meshlets. An empty range, which draws no meshlets,
    /// when the buffers cannot grow to hold them; the reason is logged.
    MeshletRange upload(const MeshletBuildResult& result, uint32_t mega_vertex_offset);

    [[nodiscard]] gpu::Buffer meshlet_buffer() const { return meshlets_; }
    [[nodiscard]] gpu::Buffer bounds_buffer() const { return bounds_; }
    [[nodiscard]] gpu::Buffer vertex_index_buffer() const { return vertex_indices_; }
    [[nodiscard]] gpu::Buffer triangle_buffer() const { return triangles_; }

    // The bytes of each uploaded so far.
    [[nodiscard]] uint64_t meshlet_buffer_size() const { return meshlet_used_; }
    [[nodiscard]] uint64_t bounds_buffer_size() const { return bounds_used_; }
    [[nodiscard]] uint64_t vertex_index_buffer_size() const { return vertex_index_used_; }
    [[nodiscard]] uint64_t triangle_buffer_size() const { return triangle_used_; }

    [[nodiscard]] uint32_t total_meshlets() const { return meshlet_count_; }

private:
    gpu::Device& gpu_;

    gpu::Owned<gpu::Buffer> meshlets_;
    gpu::Owned<gpu::Buffer> bounds_;
    gpu::Owned<gpu::Buffer> vertex_indices_;
    gpu::Owned<gpu::Buffer> triangles_;

    uint64_t meshlet_used_{0};
    uint64_t bounds_used_{0};
    uint64_t vertex_index_used_{0};
    uint64_t triangle_used_{0};          // up to the end of the word the last byte is in
    uint64_t triangle_raw_bytes_{0};     // bytes of triangles (for offset calculation)

    uint32_t meshlet_count_{0};

    static constexpr uint64_t INITIAL_MESHLET_SIZE = 256 * 1024;     // ~16K meshlets
    static constexpr uint64_t INITIAL_BOUNDS_SIZE = 768 * 1024;       // ~16K bounds
    static constexpr uint64_t INITIAL_VERTEX_INDEX_SIZE = 1024 * 1024; // ~256K indices
    static constexpr uint64_t INITIAL_TRIANGLE_SIZE = 512 * 1024;     // ~170K triangles
    // What each may grow to; where the device grows buffers in place the
    // range is kept up front and memory added as it grows.
    static constexpr uint64_t MAX_MESHLET_SIZE = 128ULL * 1024 * 1024;
    static constexpr uint64_t MAX_BOUNDS_SIZE = 256ULL * 1024 * 1024;
    static constexpr uint64_t MAX_VERTEX_INDEX_SIZE = 512ULL * 1024 * 1024;
    static constexpr uint64_t MAX_TRIANGLE_SIZE = 256ULL * 1024 * 1024;
};

} // namespace fjell
