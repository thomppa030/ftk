#include "renderer/resources/meshlet_descriptor.hpp"
#include "renderer/resources/mega_buffer.hpp"
#include "renderer/resources/meshlet_buffer.hpp"
#include "ftk/base/log.hpp"
#include "ftk/gpu/device.hpp"

#include <algorithm>
#include <string>
#include <string_view>

namespace fjell {

namespace {

// Bytes a binding reads at the least: never an empty range.
constexpr uint64_t MIN_RANGE = 16;

constexpr std::array<std::string_view, 6> BINDINGS{
    "meshlets", "bounds", "vertex_indices", "triangles", "vertices", "visible_instances",
};

} // namespace

Result<> MeshletDescriptor::create(gpu::Device& gpu) {
    gpu_ = &gpu;
    gpu::SharedLayoutDesc desc{.name = "meshlets", .usual_set = 3, .bindings = {}};
    for (uint32_t i = 0; i < BINDINGS.size(); ++i) {
        gpu::ShaderBinding binding;
        binding.name = std::string(BINDINGS[i]);
        binding.binding = i;
        binding.kind = gpu::BindingKind::storage_buffer;
        binding.stages = gpu::ShaderStage::task | gpu::ShaderStage::mesh;
        desc.bindings.push_back(std::move(binding));
    }
    auto layout = gpu.create(desc);
    if (!layout) return make_error(layout.error());
    layout_ = *layout;

    auto empty = gpu.create(gpu::BufferDesc{
        .size = MIN_RANGE, .use = gpu::BufferUse::storage, .name = "meshlets_empty"});
    if (!empty) return make_error(empty.error());
    empty_ = std::move(*empty);

    for (auto& group : groups_) {
        gpu::SharedGroupDesc group_desc{.layout = layout_, .entries = {}, .name = "meshlets"};
        for (std::string_view name : BINDINGS) group_desc.entries.push_back({name, gpu::storage(empty_)});
        auto made = gpu.create(group_desc);
        if (!made) return make_error(made.error());
        group = std::move(*made);
    }
    FJELL_GFX_DEBUG("Meshlet descriptor created (set 3, {} storage buffers)", BINDINGS.size());
    return {};
}

void MeshletDescriptor::destroy() {
    for (auto& group : groups_) group.reset();
    empty_.reset();
}

void MeshletDescriptor::update(const MeshletBuffer& meshlet_buf,
                               const MegaBuffer& mega_buf,
                               uint32_t frame_index,
                               gpu::Buffer visible_buffer,
                               uint64_t visible_size) {
    // Each bound up to what is uploaded, and never empty.
    const auto whole = [&](gpu::Buffer buffer, uint64_t size) {
        if (!buffer.valid()) return gpu::storage(empty_);
        return gpu::storage(gpu::BufferRange(buffer, 0, std::max(size, MIN_RANGE)));
    };
    const gpu::BindEntry entries[] = {
        {BINDINGS[0], whole(meshlet_buf.meshlet_buffer(), meshlet_buf.meshlet_buffer_size())},
        {BINDINGS[1], whole(meshlet_buf.bounds_buffer(), meshlet_buf.bounds_buffer_size())},
        {BINDINGS[2], whole(meshlet_buf.vertex_index_buffer(), meshlet_buf.vertex_index_buffer_size())},
        {BINDINGS[3], whole(meshlet_buf.triangle_buffer(), meshlet_buf.triangle_buffer_size())},
        {BINDINGS[4], whole(mega_buf.vertex_buffer(), mega_buf.vertex_buffer_size())},
        // The meshlet cull's visible instances, or the empty buffer while
        // the GPU-driven path has none.
        {BINDINGS[5], visible_buffer.valid() && visible_size > 0
                          ? gpu::storage(gpu::BufferRange(visible_buffer, 0, visible_size))
                          : gpu::storage(empty_)},
    };
    if (auto written = gpu_->update(groups_[frame_index], entries); !written) {
        FJELL_GFX_ERROR("Meshlet set: {}", written.error());
    }
}

} // namespace fjell
