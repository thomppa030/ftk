#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>

namespace ftk::gpu {

/// The bookkeeping of a staging ring: one buffer that uploads are copied
/// through, used round and round. Live regions run from the tail to the head,
/// possibly wrapped; each belongs to the batch of commands that copies out of
/// it, and regions free in ring order once their batch has completed. The
/// ring holds no memory: its owner maps the buffer, records the copies and
/// submits the batches, and the ring says where each upload goes.
class StagingRing {
public:
    /// When a region's batch is done: the value its lane's timeline reaches
    /// on completing it, 0 while the batch is still being recorded.
    using BatchValue = std::shared_ptr<const uint64_t>;

    /// What placing asks of the ring's owner.
    struct Owner {
        /// The value `lane`'s timeline has reached.
        std::function<uint64_t(uint32_t lane)> completed;
        /// `lane`'s open batch, opened if it is not.
        std::function<BatchValue(uint32_t lane)> open_batch;
        /// Submits every open batch, giving each its value, and frees what
        /// has completed (reclaim()).
        std::function<void()> flush;
        /// Waits until `lane`'s timeline reaches `value`.
        std::function<void(uint32_t lane, uint64_t value)> wait;
    };

    /// A ring of `capacity` bytes whose regions start on multiples of
    /// `alignment`, a power of two.
    StagingRing(uint64_t capacity, uint64_t alignment) : capacity_{capacity}, alignment_{alignment} {}

    /// Places `size` bytes, at most half the ring, for `lane`'s open batch
    /// and returns where they start. A full ring submits what it has; if
    /// that frees nothing, it waits for the oldest batch. Throws when the
    /// oldest region belongs to a batch still open, which nothing completes.
    [[nodiscard]] uint64_t place(uint64_t size, uint32_t lane, const Owner& owner);

    /// Frees the regions whose batches have completed, in ring order: a
    /// region waits for every region placed before it.
    void reclaim(const std::function<uint64_t(uint32_t lane)>& completed);

    [[nodiscard]] uint64_t capacity() const { return capacity_; }
    /// Regions not yet freed, the end-of-ring padding a wrap leaves included.
    [[nodiscard]] std::size_t live() const { return regions_.size(); }

private:
    struct Region {
        uint64_t end{0};
        uint32_t lane{0};
        BatchValue batch;
    };

    uint64_t capacity_;
    uint64_t alignment_;
    uint64_t head_{0};
    uint64_t tail_{0};
    std::deque<Region> regions_;
};

} // namespace ftk::gpu
