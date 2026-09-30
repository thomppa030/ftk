#pragma once

#include "core/result.hpp"
#include "gpu/binding.hpp"
#include "gpu/buffer.hpp"
#include "gpu/owned.hpp"
#include "renderer/renderer_constants.hpp"

#include <array>
#include <cstdint>

namespace fjell {

class MeshletBuffer;
class MegaBuffer;
namespace gpu {
class Device;
}

/// What the meshlet task and mesh shaders read at set 3: the meshlets, their
/// bounds, vertex indices and packed triangles, the mega buffer's vertices,
/// and the meshlet cull's visible instances. A group per frame slot, every
/// binding holding a small empty buffer until the frame gives it one.
class MeshletDescriptor {
public:
    /// Makes the layout and the groups.
    /// @return nothing, or what could not be made.
    [[nodiscard]] Result<> create(gpu::Device& gpu);
    void destroy();

    /// Binds the meshlets and the mega buffer's vertices into the frame
    /// slot's group, and the meshlet cull's visible instances, or the empty
    /// buffer while there are none.
    void update(const MeshletBuffer& meshlet_buf, const MegaBuffer& mega_buf,
                uint32_t frame_index,
                gpu::Buffer visible_buffer = {},
                uint64_t visible_size = 0);

    /// The layout pipelines name to bind the groups.
    [[nodiscard]] gpu::SharedLayout layout() const { return layout_; }
    /// The group the frames of `frame_index`'s slot bind.
    [[nodiscard]] gpu::BindGroup group(uint32_t frame_index) const { return groups_[frame_index].get(); }

private:
    gpu::Device* gpu_{nullptr};
    gpu::SharedLayout layout_{};
    gpu::Owned<gpu::Buffer> empty_;
    std::array<gpu::Owned<gpu::BindGroup>, MAX_FRAMES_IN_FLIGHT> groups_;
};

} // namespace fjell
