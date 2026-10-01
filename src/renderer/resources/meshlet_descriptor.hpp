#pragma once

#include "ftk/base/result.hpp"
#include "ftk/gpu/binding.hpp"
#include "ftk/gpu/buffer.hpp"
#include "ftk/gpu/owned.hpp"
#include "renderer/renderer_constants.hpp"

#include <array>
#include <cstdint>

namespace ftk::gpu {
class Device;
} // namespace ftk::gpu

namespace fjell {

class MeshletBuffer;
class MegaBuffer;

/// What the meshlet task and mesh shaders read at set 3: the meshlets, their
/// bounds, vertex indices and packed triangles, the mega buffer's vertices,
/// and the meshlet cull's visible instances. A group per frame slot, every
/// binding holding a small empty buffer until the frame gives it one.
class MeshletDescriptor {
public:
    /// Makes the layout and the groups.
    /// @return nothing, or what could not be made.
    [[nodiscard]] ftk::Result<> create(ftk::gpu::Device& gpu);
    void destroy();

    /// Binds the meshlets and the mega buffer's vertices into the frame
    /// slot's group, and the meshlet cull's visible instances, or the empty
    /// buffer while there are none.
    void update(const MeshletBuffer& meshlet_buf, const MegaBuffer& mega_buf,
                uint32_t frame_index,
                ftk::gpu::Buffer visible_buffer = {},
                uint64_t visible_size = 0);

    /// The layout pipelines name to bind the groups.
    [[nodiscard]] ftk::gpu::SharedLayout layout() const { return layout_; }
    /// The group the frames of `frame_index`'s slot bind.
    [[nodiscard]] ftk::gpu::BindGroup group(uint32_t frame_index) const { return groups_[frame_index].get(); }

private:
    ftk::gpu::Device* gpu_{nullptr};
    ftk::gpu::SharedLayout layout_{};
    ftk::gpu::Owned<ftk::gpu::Buffer> empty_;
    std::array<ftk::gpu::Owned<ftk::gpu::BindGroup>, ftk::MAX_FRAMES_IN_FLIGHT> groups_;
};

} // namespace fjell
