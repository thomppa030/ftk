#include "renderer/resources/mega_buffer.hpp"
#include "ftk/gpu/render_encoder.hpp"
#include "ftk/gpu/device.hpp"
#include "ftk/base/log.hpp"

#include <stdexcept>

namespace fjell {

namespace {

// Vertices and indices are drawn from, read by the meshlet and ray-tracing
// shaders as storage, and built into acceleration structures by address.
constexpr gpu::BufferUses MEGA_USE = gpu::BufferUse::storage | gpu::BufferUse::acceleration_input |
                                     gpu::BufferUse::device_address;

gpu::Owned<gpu::Buffer> make_growable(gpu::Device& gpu, uint64_t size, uint64_t reserve,
                                      gpu::BufferUses use, std::string_view name) {
    auto made = gpu.create(gpu::BufferDesc{.size = size, .reserve = reserve, .use = use, .name = name});
    if (!made) throw std::runtime_error(made.error());
    return std::move(*made);
}

} // namespace

MegaBuffer::MegaBuffer(gpu::Device& gpu)
    : gpu_{gpu}
    , vertices_{make_growable(gpu, INITIAL_VERTEX_SIZE, MAX_VERTEX_SIZE,
                              MEGA_USE | gpu::BufferUse::vertex, "mega_vertex")}
    , indices_{make_growable(gpu, INITIAL_INDEX_SIZE, MAX_INDEX_SIZE,
                             MEGA_USE | gpu::BufferUse::index, "mega_index")} {
    FJELL_GFX_INFO("MegaBuffer created: {}MB vertex, {}MB index",
                   gpu_.size(vertices_) / (1024 * 1024), gpu_.size(indices_) / (1024 * 1024));
}

MegaMeshEntry MegaBuffer::upload(const std::vector<Vertex>& vertices,
                                  const std::vector<uint32_t>& indices) {
    const uint64_t vertex_byte_offset = vertex_buffer_size();
    const uint64_t index_byte_offset = index_buffer_size();

    auto grown = gpu_.grow(vertices_, vertex_byte_offset + vertices.size() * sizeof(Vertex));
    if (grown) grown = gpu_.grow(indices_, index_byte_offset + indices.size() * sizeof(uint32_t));
    if (!grown) {
        FJELL_GFX_ERROR("Mesh not added to the mega buffer: {}", grown.error());
        return {};
    }

    gpu_.upload().to_buffer(vertices_, vertex_byte_offset, vertices);
    gpu_.upload().to_buffer(indices_, index_byte_offset, indices);

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
    const uint64_t byte_offset = uint64_t{entry.vertex_offset} * sizeof(Vertex);
    gpu_.upload().to_buffer(vertices_, byte_offset, vertices);
}

void MegaBuffer::bind(gpu::RenderEncoder& pass) const {
    pass.set_vertex_buffer(gpu::BufferRange(vertices_));
    pass.set_index_buffer(gpu::BufferRange(indices_), gpu::IndexType::u32);
}

uint32_t MegaBuffer::generation() const {
    return gpu_.generation(vertices_) + gpu_.generation(indices_);
}

} // namespace fjell
