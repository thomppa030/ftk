#include "core/small_vector.hpp"

#include <catch2/catch_test_macros.hpp>

#include <numeric>
#include <span>

using fjell::SmallVector;

namespace {

struct Pair {
    int a;
    int b;
};

template <size_t N>
SmallVector<Pair, N> counted(int count) {
    SmallVector<Pair, N> values;
    for (int i = 0; i < count; ++i) values.push_back({i, i * 10});
    return values;
}

template <size_t N>
bool holds_count(const SmallVector<Pair, N>& values, int count) {
    if (values.size() != static_cast<size_t>(count)) return false;
    for (int i = 0; i < count; ++i) {
        if (values[i].a != i || values[i].b != i * 10) return false;
    }
    return true;
}

} // namespace

TEST_CASE("A small vector keeps its first elements inline", "[core][small_vector]") {
    auto values = counted<4>(4);
    CHECK(holds_count(values, 4));
    CHECK_FALSE(values.spilled());
    CHECK(values.back().a == 3);
}

TEST_CASE("A small vector goes to the heap past its inline places", "[core][small_vector]") {
    auto values = counted<4>(11);
    CHECK(values.spilled());
    CHECK(holds_count(values, 11));
}

TEST_CASE("A small vector copies and moves inline and spilled", "[core][small_vector]") {
    for (int count : {0, 3, 4, 9}) {
        INFO("count " << count);
        const auto original = counted<4>(count);

        SmallVector<Pair, 4> copy(original);
        CHECK(holds_count(copy, count));
        SmallVector<Pair, 4> assigned = counted<4>(2);
        assigned = original;
        CHECK(holds_count(assigned, count));

        SmallVector<Pair, 4> moved(std::move(copy));
        CHECK(holds_count(moved, count));
        CHECK(copy.empty());
        SmallVector<Pair, 4> move_assigned = counted<4>(7);
        move_assigned = std::move(moved);
        CHECK(holds_count(move_assigned, count));
        CHECK(holds_count(original, count));
    }
}

TEST_CASE("A cleared small vector starts inline again", "[core][small_vector]") {
    auto values = counted<2>(5);
    REQUIRE(values.spilled());
    values.clear();
    CHECK(values.empty());
    CHECK_FALSE(values.spilled());
    values.push_back({7, 70});
    CHECK(values.size() == 1);
    CHECK(values[0].b == 70);
}

TEST_CASE("A small vector reads as a span and from a list", "[core][small_vector]") {
    const SmallVector<int, 3> values{1, 2, 3, 4};
    const std::span<const int> view = values;
    CHECK(view.size() == 4);
    CHECK(std::accumulate(view.begin(), view.end(), 0) == 10);
    int sum = 0;
    for (int value : values) sum += value;
    CHECK(sum == 10);
}
