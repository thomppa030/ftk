#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <utility>

namespace ftk::gpu {

/// Destruction that waits for the GPU. A backend keeps one: what it retires
/// stays alive until the GPU's timeline, the count every finished submission
/// advances, reaches the value given with it, which is the value of the last
/// submission that may still use it. One-shot submits and every window's
/// frames release the same way, since nothing here counts frame slots.
///
/// Values are expected in the order the timeline advances; one retired out
/// of order is simply kept until those before it go.
///
/// @code
/// queue.retire(next_submission_value, std::move(buffer));
/// ...
/// queue.collect(completed_value);   // destroys what the GPU is done with
/// @endcode
class ReleaseQueue {
public:
    /// Keeps `object` alive until `collect()` sees `after` completed, then
    /// lets its destructor run.
    template <typename T>
    void retire(uint64_t after, T object) {
        defer(after, [doomed = std::move(object)] { (void)doomed; });
    }

    /// Runs `fn` once `collect()` sees `after` completed: for native handles
    /// whose destruction needs more than a destructor.
    void defer(uint64_t after, std::move_only_function<void()> fn);

    /// Runs everything retired at `completed` or earlier, in the order it was
    /// retired. What those destructors retire in turn is kept until the
    /// timeline reaches the value they give.
    void collect(uint64_t completed);

    /// Runs everything, whatever it was retired at. The GPU must be idle.
    void flush();

    /// How many retirements are waiting.
    [[nodiscard]] size_t pending() const { return entries_.size(); }
    /// The latest value `collect()` has run everything up to: what was
    /// retired at it, or before, is gone.
    [[nodiscard]] uint64_t collected() const { return collected_; }

private:
    struct Entry {
        uint64_t after{0};
        std::move_only_function<void()> fn;
    };

    std::deque<Entry> entries_;
    uint64_t collected_{0};
};

/// The frame a release made now waits for, from the serial of the frame
/// recording now and of the newest frame sent to the GPU. A frame is sent
/// after every upload recorded before its end, and waits for them, so it is
/// the frame recording while one is open; with none open (before the first
/// frame, or between two) the next one, which sends the uploads recorded
/// since.
[[nodiscard]] constexpr uint64_t release_frame(uint64_t recording, uint64_t ended) {
    return recording > ended ? recording : recording + 1;
}

} // namespace ftk::gpu
