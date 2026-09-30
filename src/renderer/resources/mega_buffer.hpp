#pragma once

#include "renderer/resources/vertex.hpp"

#include "gpu/buffer.hpp"
#include "gpu/owned.hpp"

#include <cstdint>
#include <vector>

namespace fjell {

namespace gpu {
class Device;
class RenderEncoder;
}

struct MegaMeshEntry {
    uint32_t vertex_offset{0};
    uint32_t vertex_count{0};
    uint32_t first_index{0};
    uint32_t index_count{0};
    uint32_t meshlet_offset{0};
    uint32_t meshlet_count{0};
};

/// Every static mesh's vertices and 32-bit indices, one after another, so
/// every draw binds the same two buffers. Both grow as meshes are added,
/// keeping their handles.
class MegaBuffer {
public:
    /// Throws when the buffers cannot be made.
    explicit MegaBuffer(gpu::Device& gpu);

    /// Appends a mesh. An entry with no vertices and no indices, which
    /// draws nothing, when the buffers cannot grow to hold it; the reason
    /// is logged.
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

    /// Sets the vertices and 32-bit indices every mega entry draws from.
    void bind(gpu::RenderEncoder& pass) const;

    [[nodiscard]] uint32_t vertex_count() const { return vertex_offset_; }
    [[nodiscard]] uint32_t index_count() const { return index_offset_; }
    [[nodiscard]] gpu::Buffer vertex_buffer() const { return vertices_; }
    /// The bytes of vertices uploaded so far.
    [[nodiscard]] uint64_t vertex_buffer_size() const { return uint64_t{vertex_offset_} * sizeof(Vertex); }
    [[nodiscard]] gpu::Buffer index_buffer() const { return indices_; }
    /// The bytes of indices uploaded so far.
    [[nodiscard]] uint64_t index_buffer_size() const { return uint64_t{index_offset_} * sizeof(uint32_t); }

    /// Moves every time growing the vertex or index buffer moved its address,
    /// which invalidates any consumer that baked the old one — notably
    /// ray-tracing BLAS geometry. Such consumers stamp this value and rebuild
    /// when it changes. Where the device grows buffers in place it never
    /// moves.
    [[nodiscard]] uint32_t generation() const;

private:
    gpu::Device& gpu_;
    gpu::Owned<gpu::Buffer> vertices_;
    gpu::Owned<gpu::Buffer> indices_;
    uint32_t vertex_offset_{0};
    uint32_t index_offset_{0};

    static constexpr uint64_t INITIAL_VERTEX_SIZE = 16 * 1024 * 1024;
    static constexpr uint64_t INITIAL_INDEX_SIZE = 8 * 1024 * 1024;
    // What each may grow to; where the device grows buffers in place the
    // range is kept up front and memory added as it grows.
    static constexpr uint64_t MAX_VERTEX_SIZE = 1024ULL * 1024 * 1024;
    static constexpr uint64_t MAX_INDEX_SIZE = 512ULL * 1024 * 1024;
};

} // namespace fjell
