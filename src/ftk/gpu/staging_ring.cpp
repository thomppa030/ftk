#include "ftk/gpu/staging_ring.hpp"

#include <array>
#include <optional>
#include <stdexcept>

namespace ftk::gpu {

uint64_t StagingRing::place(uint64_t size, uint32_t lane, const Owner& owner) {
    // The batch exists before any region refers to it.
    (void)owner.open_batch(lane);
    const uint64_t aligned = (size + alignment_ - 1) & ~(alignment_ - 1);

    // Live data is [tail_, head_), possibly wrapped. No regions means the
    // ring is idle; equal cursors with regions mean it is full.
    for (;;) {
        reclaim(owner.completed);

        if (regions_.empty()) {
            head_ = 0;
            tail_ = 0;
            break;
        }
        if (head_ > tail_) {
            if (capacity_ - head_ >= aligned) break;
            if (tail_ > aligned) {
                // Wrap: the dead end of the ring goes to the open batch, so
                // the tail walks past it in order. Flushing may have retired
                // that batch since it was opened, so it is asked for again.
                regions_.push_back({capacity_, lane, owner.open_batch(lane)});
                head_ = 0;
                break;
            }
        } else if (head_ < tail_ && tail_ - head_ > aligned) {
            break;
        }

        // Full or fragmented: submit what there is. Flushing frees the
        // batches already done; if it freed any, look for room again, since
        // the ring may now be idle and there would be no oldest to wait for.
        const std::size_t live = regions_.size();
        owner.flush();
        if (regions_.size() != live) continue;
        const Region& oldest = regions_.front();
        if (*oldest.batch == 0) {
            throw std::runtime_error("Staging ring stuck: the oldest region belongs to a batch never submitted");
        }
        owner.wait(oldest.lane, *oldest.batch);
    }

    const uint64_t offset = head_;
    head_ += aligned;
    // Flushing may have retired this lane's batch: asked for again.
    regions_.push_back({head_, lane, owner.open_batch(lane)});
    return offset;
}

void StagingRing::reclaim(const std::function<uint64_t(uint32_t lane)>& completed) {
    // Each lane's timeline is read once, however many regions it holds.
    constexpr std::size_t MAX_LANES = 8;
    std::array<std::optional<uint64_t>, MAX_LANES> reached{};
    const auto reached_by = [&](uint32_t lane) {
        if (lane >= MAX_LANES) return completed(lane);
        if (!reached[lane]) reached[lane] = completed(lane);
        return *reached[lane];
    };
    while (!regions_.empty()) {
        const Region& front = regions_.front();
        const uint64_t value = *front.batch;
        if (value == 0 || reached_by(front.lane) < value) break;
        tail_ = front.end == capacity_ ? 0 : front.end;
        regions_.pop_front();
    }
}

} // namespace ftk::gpu
