#include "renderer/draw_data_descriptor.hpp"
#include "ftk/base/log.hpp"
#include "ftk/gpu/device.hpp"

namespace fjell {

namespace {

constexpr std::string_view DRAWS = "draws";
constexpr std::string_view BONES = "bones";
constexpr std::string_view INSTANCES = "instance_transforms";
constexpr std::string_view VISIBLE_INSTANCES = "visible_instance_transforms";
constexpr std::string_view SPLINE_SEGMENTS = "spline_segments";

ftk::gpu::ShaderBinding storage_binding(std::string_view name, uint32_t binding, ftk::gpu::ShaderStages stages) {
    ftk::gpu::ShaderBinding b;
    b.name = std::string(name);
    b.binding = binding;
    b.kind = ftk::gpu::BindingKind::storage_buffer;
    b.stages = stages;
    return b;
}

} // namespace

ftk::Result<ftk::gpu::SharedLayout> DrawDataDescriptor::make_layout(ftk::gpu::Device& gpu) {
    using ftk::gpu::ShaderStage;
    return gpu.create(ftk::gpu::SharedLayoutDesc{
        .name = "draw data",
        .usual_set = 2,
        .bindings = {
            storage_binding(DRAWS, 0, ShaderStage::vertex | ShaderStage::fragment | ShaderStage::task |
                                          ShaderStage::mesh | ShaderStage::compute),
            storage_binding(BONES, 1, ShaderStage::vertex | ShaderStage::mesh | ShaderStage::compute),
            // The raw transforms feed the mesh shader path, the visible ones
            // (per-instance culled) the vertex path.
            storage_binding(INSTANCES, 2, ShaderStage::vertex | ShaderStage::mesh | ShaderStage::task |
                                              ShaderStage::compute),
            storage_binding(VISIBLE_INSTANCES, 3, ShaderStage::vertex | ShaderStage::compute),
            storage_binding(SPLINE_SEGMENTS, 4, ShaderStage::vertex | ShaderStage::mesh),
        },
    });
}

ftk::Result<> DrawDataDescriptor::create(ftk::gpu::Device& gpu, ftk::gpu::SharedLayout layout, ftk::gpu::Buffer empty) {
    gpu_ = &gpu;
    empty_ = empty;
    for (auto& group : groups_) {
        auto made = gpu.create(ftk::gpu::SharedGroupDesc{
            .layout = layout,
            .entries = {{DRAWS, ftk::gpu::storage(empty)},
                        {BONES, ftk::gpu::storage(empty)},
                        {INSTANCES, ftk::gpu::storage(empty)},
                        {VISIBLE_INSTANCES, ftk::gpu::storage(empty)},
                        {SPLINE_SEGMENTS, ftk::gpu::storage(empty)}},
            .name = "draw_data",
        });
        if (!made) return ftk::make_error(made.error());
        group = std::move(*made);
    }
    return {};
}

void DrawDataDescriptor::destroy() {
    for (auto& group : groups_) group.reset();
}

void DrawDataDescriptor::write(std::string_view binding, const ftk::gpu::BufferRange& range, uint32_t frame_index) {
    const ftk::gpu::BindEntry entry[] = {
        {binding, ftk::gpu::storage(range.buffer.valid() && range.size > 0 ? range : ftk::gpu::BufferRange(empty_))}};
    if (auto written = gpu_->update(groups_[frame_index], entry); !written) {
        FTK_GFX_ERROR("Draw data '{}': {}", binding, written.error());
    }
}

void DrawDataDescriptor::update(const ftk::gpu::BufferRange& range, uint32_t frame_index) {
    write(DRAWS, range, frame_index);
}

void DrawDataDescriptor::update_bones(const ftk::gpu::BufferRange& range, uint32_t frame_index) {
    write(BONES, range, frame_index);
}

void DrawDataDescriptor::update_instance_transforms(const ftk::gpu::BufferRange& range, uint32_t frame_index) {
    write(INSTANCES, range, frame_index);
}

void DrawDataDescriptor::update_visible_instance_transforms(const ftk::gpu::BufferRange& range, uint32_t frame_index) {
    write(VISIBLE_INSTANCES, range, frame_index);
}

void DrawDataDescriptor::update_spline_segments(const ftk::gpu::BufferRange& range, uint32_t frame_index) {
    write(SPLINE_SEGMENTS, range, frame_index);
}

} // namespace fjell
