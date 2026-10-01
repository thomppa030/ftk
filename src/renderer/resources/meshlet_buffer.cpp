#include "renderer/resources/meshlet_buffer.hpp"
#include "ftk/gpu/device.hpp"
#include "ftk/base/log.hpp"

#include <stdexcept>
#include <vector>

namespace fjell {

namespace {

gpu::Owned<gpu::Buffer> make_growable(gpu::Device& gpu, uint64_t size, uint64_t reserve,
                                      std::string_view name) {
    auto made = gpu.create(gpu::BufferDesc{
        .size = size, .reserve = reserve, .use = gpu::BufferUse::storage, .name = name});
    if (!made) throw std::runtime_error(made.error());
    return std::move(*made);
}

} // namespace

MeshletBuffer::MeshletBuffer(gpu::Device& gpu)
    : gpu_{gpu}
    , meshlets_{make_growable(gpu, INITIAL_MESHLET_SIZE, MAX_MESHLET_SIZE, "meshlets")}
    , bounds_{make_growable(gpu, INITIAL_BOUNDS_SIZE, MAX_BOUNDS_SIZE, "meshlet_bounds")}
    , vertex_indices_{make_growable(gpu, INITIAL_VERTEX_INDEX_SIZE, MAX_VERTEX_INDEX_SIZE, "meshlet_verts")}
    , triangles_{make_growable(gpu, INITIAL_TRIANGLE_SIZE, MAX_TRIANGLE_SIZE, "meshlet_tris")} {
    FJELL_GFX_INFO("MeshletBuffer created: {}KB meshlets, {}KB bounds, {}KB vert_idx, {}KB tris",
                   gpu_.size(meshlets_) / 1024, gpu_.size(bounds_) / 1024,
                   gpu_.size(vertex_indices_) / 1024, gpu_.size(triangles_) / 1024);
}

MeshletRange MeshletBuffer::upload(const MeshletBuildResult& result,
                                   uint32_t mega_vertex_offset) {
    if (result.meshlets.empty()) return {};

    const uint64_t meshlet_size = result.meshlets.size() * sizeof(Meshlet);
    const uint64_t bounds_size = result.bounds.size() * sizeof(MeshletBounds);
    const uint64_t vert_idx_size = result.meshlet_vertices.size() * sizeof(uint32_t);

    // Triangles are three bytes each, packed back to back across meshes and
    // read as uint32 words, so the buffer reaches the word the last byte is
    // in.
    const uint64_t tri_size_raw = result.meshlet_triangles.size();
    const uint64_t tri_end_word = (triangle_raw_bytes_ + tri_size_raw + 3) & ~uint64_t{3};

    auto grown = gpu_.grow(meshlets_, meshlet_used_ + meshlet_size);
    if (grown) grown = gpu_.grow(bounds_, bounds_used_ + bounds_size);
    if (grown) grown = gpu_.grow(vertex_indices_, vertex_index_used_ + vert_idx_size);
    if (grown) grown = gpu_.grow(triangles_, tri_end_word);
    if (!grown) {
        FJELL_GFX_ERROR("Meshlets not added to the meshlet buffer: {}", grown.error());
        return {};
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

    gpu::Upload& upload = gpu_.upload();
    upload.to_buffer(meshlets_, meshlet_used_, adjusted_meshlets);
    upload.to_buffer(bounds_, bounds_used_, result.bounds);
    upload.to_buffer(vertex_indices_, vertex_index_used_, result.meshlet_vertices);

    // Only the mesh's own bytes: padding written after them would land on
    // the next mesh's first bytes, in a copy nothing orders against its.
    upload.to_buffer(triangles_, triangle_raw_bytes_, result.meshlet_triangles);

    MeshletRange range{};
    range.meshlet_offset = meshlet_count_;
    range.meshlet_count = static_cast<uint32_t>(result.meshlets.size());
    range.vertex_offset = mega_vertex_offset;

    meshlet_count_ += range.meshlet_count;
    meshlet_used_ += meshlet_size;
    bounds_used_ += bounds_size;
    vertex_index_used_ += vert_idx_size;
    triangle_raw_bytes_ += tri_size_raw;
    triangle_used_ = tri_end_word;

    return range;
}

} // namespace fjell
