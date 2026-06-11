#include "renderer/resources/meshlet_buffer.hpp"
#include "renderer/gpu/deferred_deleter.hpp"
#include "renderer/gpu/upload_context.hpp"
#include "core/log.hpp"

#include <cstring>
#include <stdexcept>

namespace fjell {

MeshletBuffer::MeshletBuffer(VmaAllocator allocator, UploadContext& upload,
                             DeferredDeleter& deleter)
    : allocator_{allocator}
    , upload_{upload}
    , deleter_{deleter}
    , meshlet_capacity_{INITIAL_MESHLET_SIZE}
    , bounds_capacity_{INITIAL_BOUNDS_SIZE}
    , vertex_index_capacity_{INITIAL_VERTEX_INDEX_SIZE}
    , triangle_capacity_{INITIAL_TRIANGLE_SIZE} {
    constexpr VkBufferUsageFlags ssbo_usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    meshlet_buffer_ = std::make_unique<Buffer>(
        allocator_, meshlet_capacity_, ssbo_usage,
        VMA_MEMORY_USAGE_AUTO, 0, 0, upload_.sharing_families());

    bounds_buffer_ = std::make_unique<Buffer>(
        allocator_, bounds_capacity_, ssbo_usage,
        VMA_MEMORY_USAGE_AUTO, 0, 0, upload_.sharing_families());

    vertex_index_buffer_ = std::make_unique<Buffer>(
        allocator_, vertex_index_capacity_, ssbo_usage,
        VMA_MEMORY_USAGE_AUTO, 0, 0, upload_.sharing_families());

    triangle_buffer_ = std::make_unique<Buffer>(
        allocator_, triangle_capacity_, ssbo_usage,
        VMA_MEMORY_USAGE_AUTO, 0, 0, upload_.sharing_families());

    FJELL_GFX_INFO("MeshletBuffer created: {}KB meshlets, {}KB bounds, {}KB vert_idx, {}KB tris",
                   meshlet_capacity_ / 1024, bounds_capacity_ / 1024,
                   vertex_index_capacity_ / 1024, triangle_capacity_ / 1024);
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

    // Grow if needed
    if (meshlet_used_ + meshlet_size > meshlet_capacity_) {
        grow_buffer(meshlet_buffer_, (meshlet_used_ + meshlet_size) * 2,
                    meshlet_used_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        meshlet_capacity_ = (meshlet_used_ + meshlet_size) * 2;
    }
    if (bounds_used_ + bounds_size > bounds_capacity_) {
        grow_buffer(bounds_buffer_, (bounds_used_ + bounds_size) * 2,
                    bounds_used_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        bounds_capacity_ = (bounds_used_ + bounds_size) * 2;
    }
    if (vertex_index_used_ + vert_idx_size > vertex_index_capacity_) {
        grow_buffer(vertex_index_buffer_, (vertex_index_used_ + vert_idx_size) * 2,
                    vertex_index_used_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        vertex_index_capacity_ = (vertex_index_used_ + vert_idx_size) * 2;
    }
    VkDeviceSize tri_end = triangle_raw_bytes_ + tri_size_padded;
    if (tri_end > triangle_capacity_) {
        grow_buffer(triangle_buffer_, tri_end * 2,
                    triangle_raw_bytes_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        triangle_capacity_ = tri_end * 2;
    }

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

    upload_.upload_buffer(meshlet_buffer_->handle(), meshlet_used_,
                          adjusted_meshlets.data(), meshlet_size);
    upload_.upload_buffer(bounds_buffer_->handle(), bounds_used_,
                          result.bounds.data(), bounds_size);
    upload_.upload_buffer(vertex_index_buffer_->handle(), vertex_index_used_,
                          result.meshlet_vertices.data(), vert_idx_size);

    // Pad triangle data to 4-byte alignment
    std::vector<uint8_t> padded_tris(tri_size_padded, 0);
    std::memcpy(padded_tris.data(), result.meshlet_triangles.data(), tri_size_raw);
    upload_.upload_buffer(triangle_buffer_->handle(), triangle_raw_bytes_,
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

void MeshletBuffer::grow_buffer(std::unique_ptr<Buffer>& buf, VkDeviceSize new_size,
                                VkDeviceSize used, VkBufferUsageFlags usage) {
    FJELL_GFX_INFO("MeshletBuffer: growing buffer to {}KB", new_size / 1024);

    auto new_buf = std::make_unique<Buffer>(
        allocator_, new_size,
        usage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VMA_MEMORY_USAGE_AUTO, 0, 0, upload_.sharing_families());

    if (used > 0) {
        upload_.copy_buffer(buf->handle(), new_buf->handle(), used);
    }

    // In-flight frames still read the old buffer — retire, don't destroy.
    deleter_.retire(std::move(buf));
    buf = std::move(new_buf);
}

} // namespace fjell
