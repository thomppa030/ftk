#pragma once

#include "gpu/release_queue.hpp"

#include <cstdint>
#include <functional>
#include <utility>

namespace fjell {

/// Deferred destruction for GPU resources, released by the frame timeline.
///
/// Destroying a buffer/image/pipeline the GPU may still be reading is a
/// use-after-free, and waiting the whole device idle to dodge that is a
/// hitch. Instead, hand the doomed object here: it stays alive until the GPU
/// has finished the frame being recorded when it was retired, as the frame
/// timeline measures it, which proves every command buffer that could
/// reference it has finished.
///
/// Main-thread only: retirements are marked with the frame recording, and
/// released once a later begin_frame() is told that frame is finished.
class DeferredDeleter {
public:
    /// Keep a movable RAII object (Buffer, unique_ptr, VmaImage, ...) alive
    /// until the GPU is provably done with it, then let its destructor run.
    template <typename T>
    void retire(T obj) {
        defer([doomed = std::move(obj)] { (void)doomed; });
    }

    /// Run an arbitrary destruction callback once the GPU is done with the
    /// frame recording now. For raw handles (VkPipeline, VkImageView, ...)
    /// whose cleanup needs a device or allocator captured alongside.
    void defer(std::move_only_function<void()> fn) {
        queue_.defer(recording_, std::move(fn));
    }

    /// Call once per frame, when its serial is known and before it records:
    /// destroys everything retired by `finished` or earlier, the newest frame
    /// the GPU has finished, and marks new retirements with `serial`.
    void begin_frame(uint64_t serial, uint64_t finished) {
        recording_ = serial;
        queue_.collect(finished);
    }

    /// Destroy everything, whatever frame retired it. The device must be idle.
    void flush_all() { queue_.flush(); }

private:
    gpu::ReleaseQueue queue_;
    uint64_t recording_{0};
};

} // namespace fjell
