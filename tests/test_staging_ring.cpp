#include "ftk/gpu/staging_ring.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <stdexcept>

using ftk::gpu::StagingRing;

namespace {

// Two upload lanes as the ring's owner sees them: each with an open batch,
// a count of batches submitted and the value its timeline has reached.
struct Lanes {
    StagingRing& ring;
    std::array<std::shared_ptr<uint64_t>, 2> open{};
    std::array<uint64_t, 2> submitted{};
    std::array<uint64_t, 2> reached{};
    // The GPU has finished what is submitted by the time anything is asked
    // again, as it has when uploads are small and the frame is light.
    bool done_when_submitted{false};
    // Submitting does nothing: a batch that is never sent.
    bool submits{true};
    int flushes{0};
    int waits{0};

    uint64_t completed(uint32_t lane) const { return reached[lane]; }

    void flush() {
        ++flushes;
        if (!submits) return;
        for (uint32_t lane = 0; lane < 2; ++lane) {
            if (!open[lane]) continue;
            *open[lane] = ++submitted[lane];
            open[lane].reset();
            if (done_when_submitted) reached[lane] = submitted[lane];
        }
        ring.reclaim([this](uint32_t lane) { return completed(lane); });
    }

    StagingRing::Owner owner() {
        return {
            .completed = [this](uint32_t lane) { return completed(lane); },
            .open_batch = [this](uint32_t lane) -> StagingRing::BatchValue {
                if (!open[lane]) open[lane] = std::make_shared<uint64_t>(0);
                return open[lane];
            },
            .flush = [this] { flush(); },
            .wait =
                [this](uint32_t lane, uint64_t value) {
                    ++waits;
                    reached[lane] = std::max(reached[lane], value);
                },
        };
    }
};

} // namespace

TEST_CASE("Uploads are placed one after another, each on the ring's alignment", "[staging_ring]") {
    StagingRing ring(1024, 16);
    Lanes lanes{.ring = ring};
    CHECK(ring.place(10, 0, lanes.owner()) == 0);
    CHECK(ring.place(20, 0, lanes.owner()) == 16);
    CHECK(ring.place(1, 1, lanes.owner()) == 48);
    CHECK(ring.live() == 3);
    CHECK(lanes.flushes == 0);
}

TEST_CASE("A full ring whose submitted uploads are already done places again without waiting",
          "[staging_ring]") {
    StagingRing ring(256, 16);
    Lanes lanes{.ring = ring, .done_when_submitted = true};
    CHECK(ring.place(100, 1, lanes.owner()) == 0);
    CHECK(ring.place(100, 1, lanes.owner()) == 112);

    // No room at the end and none to wrap into: the ring submits what it
    // has, which frees all of it, and starts again from the front.
    CHECK(ring.place(100, 1, lanes.owner()) == 0);
    CHECK(lanes.flushes == 1);
    CHECK(lanes.waits == 0);
    CHECK(ring.live() == 1);
}

TEST_CASE("A full ring waits for its oldest batch when nothing it submitted is done yet", "[staging_ring]") {
    StagingRing ring(256, 16);
    Lanes lanes{.ring = ring};
    (void)ring.place(100, 0, lanes.owner());
    (void)ring.place(100, 0, lanes.owner());

    CHECK(ring.place(100, 0, lanes.owner()) == 0);
    CHECK(lanes.flushes == 1);
    CHECK(lanes.waits == 1);
    CHECK(lanes.reached[0] == 1);
}

TEST_CASE("Regions free in ring order, a done batch waiting behind an earlier one", "[staging_ring]") {
    StagingRing ring(1024, 16);
    Lanes lanes{.ring = ring};
    (void)ring.place(64, 0, lanes.owner());
    (void)ring.place(64, 1, lanes.owner());
    lanes.flush();
    CHECK(ring.live() == 2);

    // The later region's lane finishes first: it still waits for the earlier.
    lanes.reached[1] = lanes.submitted[1];
    ring.reclaim([&](uint32_t lane) { return lanes.completed(lane); });
    CHECK(ring.live() == 2);

    lanes.reached[0] = lanes.submitted[0];
    ring.reclaim([&](uint32_t lane) { return lanes.completed(lane); });
    CHECK(ring.live() == 0);
}

TEST_CASE("Past the end of the ring an upload wraps to the front, the end left to the open batch",
          "[staging_ring]") {
    StagingRing ring(256, 16);
    Lanes lanes{.ring = ring};
    (void)ring.place(96, 0, lanes.owner()); // [0, 96)
    lanes.flush();
    (void)ring.place(96, 0, lanes.owner()); // [96, 192)
    lanes.flush();
    lanes.reached[0] = 1;                    // the first is done: the tail moves to 96
    CHECK(ring.place(64, 0, lanes.owner()) == 192); // fits at the end
    CHECK(ring.place(64, 0, lanes.owner()) == 0);   // wraps
    CHECK(lanes.waits == 0);
    // [96, 192), [192, 256), the end padding, and [0, 64).
    CHECK(ring.live() == 4);
}

TEST_CASE("A ring whose oldest batch was never submitted says so rather than waiting forever",
          "[staging_ring]") {
    StagingRing ring(256, 16);
    Lanes lanes{.ring = ring, .submits = false};
    (void)ring.place(100, 0, lanes.owner());
    (void)ring.place(100, 0, lanes.owner());
    CHECK_THROWS_AS(ring.place(100, 0, lanes.owner()), std::runtime_error);
}
