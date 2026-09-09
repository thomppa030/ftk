#include "renderer/resources/meshlet_buffer.hpp"
#include "renderer/gpu/upload_context.hpp"
#include "core/log.hpp"

#include <cstring>
#include <vector>

namespace fjell {

namespace {

constexpr VkBufferUsageFlags MESHLET_SSBO_USAGE =
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

} // namespace

MeshletBuffer::MeshletBuffer(Device& device, VmaAllocator allocator,
                             UploadContext& upload, DeferredDeleter& deleter)
    : upload_{upload}
    , meshlet_buffer_{device, allocator, upload, deleter, MESHLET_SSBO_USAGE,
                      INITIAL_MESHLET_SIZE, MAX_MESHLET_SIZE, "meshlets"}
    , bounds_buffer_{device, allocator, upload, deleter, MESHLET_SSBO_USAGE,
                     INITIAL_BOUNDS_SIZE, MAX_BOUNDS_SIZE, "meshlet_bounds"}
    , vertex_index_buffer_{device, allocator, upload, deleter, MESHLET_SSBO_USAGE,
                           INITIAL_VERTEX_INDEX_SIZE, MAX_VERTEX_INDEX_SIZE, "meshlet_verts"}
    , triangle_buffer_{device, allocator, upload, deleter, MESHLET_SSBO_USAGE,
                       INITIAL_TRIANGLE_SIZE, MAX_TRIANGLE_SIZE, "meshlet_tris"} {
    FJELL_GFX_INFO("MeshletBuffer created: {}KB meshlets, {}KB bounds, {}KB vert_idx, {}KB tris",
                   meshlet_buffer_.capacity() / 1024, bounds_buffer_.capacity() / 1024,
                   vertex_index_buffer_.capacity() / 1024, triangle_buffer_.capacity() / 1024);
}

MeshletRange MeshletBuffer::upload(const MeshletBuildResult& result,
                                   uint32_t mega_vertex_offset) {
    if (result.meshlets.empty()) return {};

    VkDeviceSize meshlet_size = result.meshlets.size() * sizeof(Meshlet);
    VkDeviceSize bounds_size = result.bounds.size() * sizeof(MeshletBounds);
    VkDeviceSize vert_idx_size = result.meshlet_vertices.size() * sizeof(uint32_t);

    // Pack uint8 triangles into uint32 for GPU access (pad to 4 bytes)
    VkDeviceSize tri_size_raw = result.meshlet_triangles.size();
    VkDeviceSize tri_size_padded = (tri_size_raw + 3) & ~3;

    meshlet_buffer_.ensure_capacity(meshlet_used_ + meshlet_size);
    bounds_buffer_.ensure_capacity(bounds_used_ + bounds_size);
    vertex_index_buffer_.ensure_capacity(vertex_index_used_ + vert_idx_size);
    triangle_buffer_.ensure_capacity(triangle_raw_bytes_ + tri_size_padded);

    // Rebase meshlet offsets from per-result local to global buffer positions
    uint32_t global_vert_idx_base = static_cast<uint32_t>(vertex_index_used_ / sizeof(uint32_t));
    // triangle_raw_bytes_ tracks actual byte count (not padded), so offset is clean
    uint32_t global_tri_base_bytes = static_cast<uint32_t>(triangle_raw_bytes_);

    std::vector<Meshlet> adjusted_meshlets = result.meshlets;
    for (auto& m : adjusted_meshlets) {
        m.vertex_offset += global_vert_idx_base;
        // Shader does: byte_offset = triangle_offset * 3 + local_tri * 3 + vert
        // So we convert global byte offset back to triangle units
        uint32_t original_byte_offset = m.triangle_offset * 3;
        m.triangle_offset = (original_byte_offset + global_tri_base_bytes) / 3;
    }

    upload_.upload_buffer(meshlet_buffer_.handle(), meshlet_used_,
                          adjusted_meshlets.data(), meshlet_size);
    upload_.upload_buffer(bounds_buffer_.handle(), bounds_used_,
                          result.bounds.data(), bounds_size);
    upload_.upload_buffer(vertex_index_buffer_.handle(), vertex_index_used_,
                          result.meshlet_vertices.data(), vert_idx_size);

    // Pad triangle data to 4-byte alignment
    std::vector<uint8_t> padded_tris(tri_size_padded, 0);
    std::memcpy(padded_tris.data(), result.meshlet_triangles.data(), tri_size_raw);
    upload_.upload_buffer(triangle_buffer_.handle(), triangle_raw_bytes_,
                          padded_tris.data(), tri_size_padded);

    MeshletRange range{};
    range.meshlet_offset = meshlet_count_;
    range.meshlet_count = static_cast<uint32_t>(result.meshlets.size());
    range.vertex_offset = mega_vertex_offset;

    meshlet_count_ += range.meshlet_count;
    meshlet_used_ += meshlet_size;
    bounds_used_ += bounds_size;
    vertex_index_used_ += vert_idx_size;
    triangle_raw_bytes_ += tri_size_raw;
    triangle_used_ = (triangle_raw_bytes_ + 3) & ~3; // padded total

    return range;
}

} // namespace fjell
