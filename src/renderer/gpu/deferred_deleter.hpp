#pragma once

#include "renderer/gpu/frames_in_flight.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace fjell {

/// Frame-fenced deferred destruction for GPU resources.
///
/// Destroying a buffer/image/pipeline the GPU may still be reading is a
/// use-after-free, and waiting the whole device idle to dodge that is a
/// hitch. Instead, hand the doomed object here: it stays alive until the
/// frame slot that retired it has its in-flight fence signal again, which
/// proves every command buffer that could reference it has finished.
///
/// Main-thread only: retirements land in the slot currently recording, and
/// reclaim happens right after that slot's fence wait in begin_frame.
class DeferredDeleter {
public:
    /// Keep a movable RAII object (Buffer, unique_ptr, VmaImage, ...) alive
    /// until the GPU is provably done with it, then let its destructor run.
    template <typename T>
    void retire(T obj) {
        defer([doomed = std::move(obj)] { (void)doomed; });
    }

    /// Run an arbitrary destruction callback once the GPU is done with the
    /// current frame slot. For raw handles (VkPipeline, VkImageView, ...)
    /// whose cleanup needs a device or allocator captured alongside.
    void defer(std::move_only_function<void()> fn) {
        buckets_[slot_].push_back(std::move(fn));
    }

    /// Call once per frame, immediately after the slot's in-flight fence
    /// wait: destroys everything retired the previous time this slot
    /// recorded, and directs new retirements at this slot.
    void begin_frame(uint32_t frame_index) {
        slot_ = frame_index;
        flush(slot_);
    }

    /// Destroy everything regardless of slot. The device must be idle.
    void flush_all() {
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            flush(i);
        }
    }

private:
    void flush(uint32_t slot) {
        // Swap out first: a callback may itself retire objects, which must
        // land in the live bucket and survive until this slot cycles again.
        std::vector<std::move_only_function<void()>> doomed;
        doomed.swap(buckets_[slot]);
        for (auto& fn : doomed) {
            fn();
        }
    }

    std::array<std::vector<std::move_only_function<void()>>, MAX_FRAMES_IN_FLIGHT> buckets_;
    uint32_t slot_{0};
};

} // namespace fjell
