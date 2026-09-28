#include "core/handle_pool.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <set>
#include <string>

using namespace fjell;

namespace {
struct ItemTag;
using ItemHandle = Handle<ItemTag>;
using Items = HandlePool<std::string, ItemTag>;
} // namespace

TEST_CASE("a pool finds what was put in it by its handle", "[handle_pool]") {
    Items items;
    const ItemHandle a = items.emplace("a");
    const ItemHandle b = items.emplace("b");
    REQUIRE(a.valid());
    REQUIRE(b.valid());
    CHECK(a != b);
    CHECK(*items.get(a) == "a");
    CHECK(*items.get(b) == "b");
    CHECK(items.size() == 2);
}

TEST_CASE("a taken object is handed back and its handle finds nothing", "[handle_pool]") {
    Items items;
    const ItemHandle a = items.emplace("a");
    const auto taken = items.take(a);
    REQUIRE(taken.has_value());
    CHECK(*taken == "a");
    CHECK(items.get(a) == nullptr);
    CHECK_FALSE(items.contains(a));
    CHECK(items.size() == 0);
    CHECK_FALSE(items.take(a).has_value());
}

TEST_CASE("a reused slot does not answer to the old handle", "[handle_pool]") {
    Items items;
    const ItemHandle old_handle = items.emplace("old");
    (void)items.take(old_handle);
    const ItemHandle new_handle = items.emplace("new");
    CHECK(new_handle.index() == old_handle.index());
    CHECK(new_handle != old_handle);
    CHECK(items.get(old_handle) == nullptr);
    CHECK(*items.get(new_handle) == "new");
}

TEST_CASE("an object stays where it is while more are added", "[handle_pool]") {
    Items items;
    const ItemHandle first = items.emplace("first");
    const std::string* where = items.get(first);
    for (int i = 0; i < 5000; ++i) (void)items.emplace(std::to_string(i));
    CHECK(items.get(first) == where);
    CHECK(*where == "first");
}

TEST_CASE("handles that were never made find nothing", "[handle_pool]") {
    Items items;
    CHECK(items.get(ItemHandle{}) == nullptr);
    CHECK(items.get(ItemHandle::make(7, 1)) == nullptr);
    (void)items.emplace("x");
    CHECK(items.get(ItemHandle::make(0, 2)) == nullptr);
}

TEST_CASE("a slot used over and over never hands out the invalid handle", "[handle_pool]") {
    Items items;
    std::set<uint32_t> ids;
    for (int i = 0; i < 5000; ++i) {
        const ItemHandle h = items.emplace("again");
        CHECK(h.valid());
        ids.insert(h.id);
        (void)items.take(h);
    }
    // One slot, and every generation but 0 was used before the count wrapped.
    CHECK(ids.size() == 4095);
}

TEST_CASE("a pool holds objects that can only move", "[handle_pool]") {
    HandlePool<std::unique_ptr<int>, ItemTag> pool;
    const ItemHandle h = pool.emplace(std::make_unique<int>(42));
    CHECK(**pool.get(h) == 42);
    auto taken = pool.take(h);
    REQUIRE(taken.has_value());
    CHECK(**taken == 42);
}
