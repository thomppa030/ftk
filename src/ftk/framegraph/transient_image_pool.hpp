#pragma once

#include "ftk/framegraph/resource_desc.hpp"
#include "ftk/gpu/owned.hpp"
#include "ftk/gpu/texture.hpp"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <vector>

namespace ftk {

namespace gpu {
class Device;
}

/// Cross-frame pool of textures matching specific TextureDesc shapes, made
/// through the GPU device. Each AliasGroup from
/// FrameGraph::compute_alias_groups() asks the pool for one texture — the
/// pool returns a free cached texture that matches exactly, or makes a
/// fresh one.
///
/// Lifecycle: a texture acquired in one frame is free again once that
/// frame's fence has been waited on, which begin_frame() is told through
/// the completed serial; a frame still in flight keeps its textures, so a
/// new frame never writes what an older one is still reading. Within a
/// frame, acquire() pulls a free matching texture (making one on miss) and
/// every run of that frame gets its own. A texture no frame has wanted
/// for a while is released in begin_frame(), once its last frame is
/// complete, which is what reclaims a resolution nothing renders at any
/// more.
class TransientImagePool {
public:
    TransientImagePool() = default;
    ~TransientImagePool() = default;

    TransientImagePool(const TransientImagePool&) = delete;
    TransientImagePool& operator=(const TransientImagePool&) = delete;
    TransientImagePool(TransientImagePool&&) = delete;
    TransientImagePool& operator=(TransientImagePool&&) = delete;

    void create(gpu::Device& device);
    void destroy();

    /// Once per frame. `frame_serial` names the frame about to record and
    /// `completed_serial` the newest frame whose fence has been waited on:
    /// textures last acquired by that frame or earlier are free again, and
    /// ones idle for STALE_FRAMES are released.
    void begin_frame(uint64_t frame_serial, uint64_t completed_serial);

    /// A texture matching the resolved desc and able to serve `uses`, made
    /// on a miss. It is the caller's until the frame begin_frame() was last
    /// told about is complete.
    /// @return the texture, or an invalid handle when none could be made
    [[nodiscard]] gpu::Texture acquire(const TextureDesc& desc, glm::uvec2 viewport_extent,
                                       gpu::TextureUses uses);

    /// The extent a texture of `desc` has at `viewport`. A persistent image
    /// that must line up with a transient of the same desc sizes itself
    /// through this rather than repeating the arithmetic.
    [[nodiscard]] static glm::uvec3 resolve_extent(const TextureDesc& desc, glm::uvec2 viewport);

    /// Frames a texture may go unused before it is released.
    static constexpr uint64_t STALE_FRAMES = 120;

    // Stats
    [[nodiscard]] uint32_t live_images() const noexcept;
    [[nodiscard]] uint32_t acquires_this_frame() const noexcept { return acquires_this_frame_; }
    [[nodiscard]] uint32_t allocations_this_frame() const noexcept { return allocations_this_frame_; }
    [[nodiscard]] uint64_t live_bytes() const noexcept { return live_bytes_; }

private:
    struct Entry {
        gpu::Owned<gpu::Texture> texture;
        uint64_t bytes{0};
        uint64_t last_used{0};  ///< frame serial of the last acquire; 0 = never
    };

    [[nodiscard]] bool entry_matches(const Entry& e, const TextureDesc& desc, glm::uvec3 resolved,
                                     gpu::TextureUses uses) const;

    gpu::Device* device_{nullptr};
    std::vector<Entry> entries_;
    uint64_t frame_serial_{0};
    uint64_t completed_serial_{0};

    uint32_t acquires_this_frame_{0};
    uint32_t allocations_this_frame_{0};
    uint64_t live_bytes_{0};
};

} // namespace ftk
