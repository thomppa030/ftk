#include "ftk/base/range_allocator.hpp"

#include <catch2/catch_test_macros.hpp>

using ftk::RangeAllocator;

TEST_CASE("ranges are placed one after another while nothing is given back", "[base][range_allocator]") {
    RangeAllocator ranges;
    CHECK(ranges.allocate(10) == 0);
    CHECK(ranges.allocate(5) == 10);
    CHECK(ranges.allocate(0) == 15);
    CHECK(ranges.end() == 15);
    CHECK(ranges.used() == 15);
    CHECK(ranges.free_units() == 0);
}

TEST_CASE("a range given back is reused by the next one that fits", "[base][range_allocator]") {
    RangeAllocator ranges;
    const uint64_t a = ranges.allocate(10);
    (void)ranges.allocate(10);
    ranges.free(a, 10);
    CHECK(ranges.free_units() == 10);

    // Too big for the gap: it goes at the end.
    CHECK(ranges.allocate(12) == 20);
    // Fits: it goes in the gap, and what is left of the gap stays free.
    CHECK(ranges.allocate(4) == 0);
    CHECK(ranges.free_units() == 6);
    CHECK(ranges.allocate(6) == 4);
    CHECK(ranges.free_units() == 0);
    CHECK(ranges.end() == 32);
}

TEST_CASE("free neighbours join into one range", "[base][range_allocator]") {
    RangeAllocator ranges;
    const uint64_t a = ranges.allocate(4);
    const uint64_t b = ranges.allocate(4);
    const uint64_t c = ranges.allocate(4);
    (void)ranges.allocate(4);  // keeps the gap off the end

    ranges.free(a, 4);
    ranges.free(c, 4);
    CHECK(ranges.free_units() == 8);
    // Two gaps of 4: a range of 12 fits neither.
    CHECK(ranges.allocate(12) == 16);
    ranges.free(16, 12);

    // The middle one joins both into one gap of 12.
    ranges.free(b, 4);
    CHECK(ranges.allocate(12) == 0);
    CHECK(ranges.free_units() == 0);
}

TEST_CASE("giving back the last ranges shortens what is in use", "[base][range_allocator]") {
    RangeAllocator ranges;
    const uint64_t a = ranges.allocate(8);
    const uint64_t b = ranges.allocate(8);
    const uint64_t c = ranges.allocate(8);

    ranges.free(b, 8);
    CHECK(ranges.end() == 24);
    // The last one goes, and the gap before it with it.
    ranges.free(c, 8);
    CHECK(ranges.end() == 8);
    CHECK(ranges.free_units() == 0);

    ranges.free(a, 8);
    CHECK(ranges.end() == 0);
    CHECK(ranges.used() == 0);
    CHECK(ranges.allocate(3) == 0);
}

TEST_CASE("clearing frees everything", "[base][range_allocator]") {
    RangeAllocator ranges;
    (void)ranges.allocate(8);
    ranges.free(ranges.allocate(8), 8);
    ranges.clear();
    CHECK(ranges.end() == 0);
    CHECK(ranges.free_units() == 0);
    CHECK(ranges.allocate(5) == 0);
}
