#pragma once

#include "gpu/buffer.hpp"
#include "gpu/owned.hpp"
#include "renderer/renderer_constants.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace fjell {

namespace gpu {
class Device;
}

/// The bone matrices of every skinned instance drawn this frame, one after
/// another, for the skinning shaders to read at each instance's offset.
/// Gathered on the CPU and written into the frame slot's buffer once, since a
/// frame still in flight reads the one before.
class BoneBuffer {
public:
    explicit BoneBuffer(gpu::Device& gpu);

    /// Starts gathering the frame's matrices anew.
    void begin_frame();

    /// Appends the bone matrices of one skinned instance.
    /// @return their offset, in matrices, from the start of the buffer.
    uint32_t upload(const std::vector<glm::mat4>& skin_matrices);

    /// Writes what the frame gathered into `frame_index`'s buffer, made anew
    /// when it is too small; the frames reading the one it replaces keep it
    /// until they are done.
    void end_frame(uint32_t frame_index);

    [[nodiscard]] gpu::Buffer buffer(uint32_t frame_index) const { return buffers_[frame_index]; }

    /// Bytes gathered this frame.
    [[nodiscard]] uint64_t byte_size() const { return matrices_.size() * sizeof(glm::mat4); }

    [[nodiscard]] uint32_t total_bones() const { return static_cast<uint32_t>(matrices_.size()); }

private:
    gpu::Device& gpu_;
    std::vector<glm::mat4> matrices_;
    std::array<gpu::Owned<gpu::Buffer>, MAX_FRAMES_IN_FLIGHT> buffers_;
    std::array<uint64_t, MAX_FRAMES_IN_FLIGHT> capacities_{};

    static constexpr uint32_t INITIAL_CAPACITY = 4096; // mat4s (~64 characters * 64 bones)
};

} // namespace fjell
