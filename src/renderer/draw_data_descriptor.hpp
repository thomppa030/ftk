#pragma once

#include "ftk/base/result.hpp"
#include "ftk/gpu/binding.hpp"
#include "ftk/gpu/buffer.hpp"
#include "ftk/gpu/owned.hpp"
#include "renderer/renderer_constants.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace ftk::gpu {
class Device;
} // namespace ftk::gpu

namespace fjell {

/// What the mesh passes read about their draws at set 2: the draw records,
/// the bone matrices, the instance transforms (raw for the mesh shader path,
/// the visible ones for the vertex path) and spline segments. The main view
/// and every shadow cascade have one, all of the same layout, with a group
/// per frame slot. A binding the frame gives nothing holds an empty buffer,
/// so no binding is ever left pointing at a buffer that has gone.
class DrawDataDescriptor {
public:
    /// The layout every draw data group is made of.
    /// @return the layout, or why it could not be made.
    [[nodiscard]] static ftk::Result<ftk::gpu::SharedLayout> make_layout(ftk::gpu::Device& gpu);

    /// A group per frame slot of `layout`, every binding holding `empty`.
    /// @return nothing, or why a group could not be made.
    [[nodiscard]] ftk::Result<> create(ftk::gpu::Device& gpu, ftk::gpu::SharedLayout layout, ftk::gpu::Buffer empty);
    void destroy();

    // Each binds `range` into the frame slot's group, as a new version of it
    // for the commands recorded after; the empty buffer while the range is
    // unset or empty.
    void update(const ftk::gpu::BufferRange& range, uint32_t frame_index);
    void update_bones(const ftk::gpu::BufferRange& range, uint32_t frame_index);
    void update_instance_transforms(const ftk::gpu::BufferRange& range, uint32_t frame_index);
    void update_visible_instance_transforms(const ftk::gpu::BufferRange& range, uint32_t frame_index);
    void update_spline_segments(const ftk::gpu::BufferRange& range, uint32_t frame_index);

    /// The group the frames of `frame_index`'s slot bind.
    [[nodiscard]] ftk::gpu::BindGroup group(uint32_t frame_index) const { return groups_[frame_index].get(); }

private:
    void write(std::string_view binding, const ftk::gpu::BufferRange& range, uint32_t frame_index);

    ftk::gpu::Device* gpu_{nullptr};
    ftk::gpu::Buffer empty_{};
    std::array<ftk::gpu::Owned<ftk::gpu::BindGroup>, ftk::MAX_FRAMES_IN_FLIGHT> groups_;
};

} // namespace fjell
