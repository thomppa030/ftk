#pragma once

#include "core/result.hpp"
#include "gpu/binding.hpp"
#include "gpu/buffer.hpp"
#include "gpu/owned.hpp"
#include "renderer/renderer_constants.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace fjell {

namespace gpu {
class Device;
}

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
    [[nodiscard]] static Result<gpu::SharedLayout> make_layout(gpu::Device& gpu);

    /// A group per frame slot of `layout`, every binding holding `empty`.
    /// @return nothing, or why a group could not be made.
    [[nodiscard]] Result<> create(gpu::Device& gpu, gpu::SharedLayout layout, gpu::Buffer empty);
    void destroy();

    // Each binds the first `size` bytes of a buffer into the frame slot's
    // group; the empty buffer while the buffer is unset or `size` is 0.
    void update(gpu::Buffer buffer, uint64_t size, uint32_t frame_index);
    void update_bones(gpu::Buffer buffer, uint64_t size, uint32_t frame_index);
    void update_instance_transforms(gpu::Buffer buffer, uint64_t size, uint32_t frame_index);
    void update_visible_instance_transforms(gpu::Buffer buffer, uint64_t size, uint32_t frame_index);
    void update_spline_segments(gpu::Buffer buffer, uint64_t size, uint32_t frame_index);

    /// The group the frames of `frame_index`'s slot bind.
    [[nodiscard]] gpu::BindGroup group(uint32_t frame_index) const { return groups_[frame_index].get(); }

private:
    void write(std::string_view binding, gpu::Buffer buffer, uint64_t size, uint32_t frame_index);

    gpu::Device* gpu_{nullptr};
    gpu::Buffer empty_{};
    std::array<gpu::Owned<gpu::BindGroup>, MAX_FRAMES_IN_FLIGHT> groups_;
};

} // namespace fjell
