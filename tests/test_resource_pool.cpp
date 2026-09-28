#include "renderer/resources/resource_pool.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>

using namespace fjell;

namespace {

struct Clip {
    int samples{0};
};

using ClipHandle = Handle<struct ClipTag>;
using ClipPool = ResourcePool<Clip, ClipHandle>;

} // namespace

TEST_CASE("a failed load keeps its path", "[resources][pool]") {
    ClipPool pool;
    const ClipHandle failed = pool.fail("sfx/coin.fjaudio");

    REQUIRE(failed.valid());
    CHECK(pool.state(failed) == LoadState::Failed);
    CHECK(pool.get(failed) == nullptr);
    CHECK(pool.path_of(failed) == "sfx/coin.fjaudio");
    CHECK(pool.find("sfx/coin.fjaudio") == failed);
    // What looks a resource up by path finds nothing to use.
    CHECK_FALSE(pool.ready("sfx/coin.fjaudio").valid());
    // Failing again is the same slot, not another.
    CHECK(pool.fail("sfx/coin.fjaudio") == failed);
    CHECK(pool.slots.size() == 1);
}

TEST_CASE("a load after a failure fills the slot the failure left", "[resources][pool]") {
    ClipPool pool;
    const ClipHandle held = pool.fail("sfx/coin.fjaudio");

    const ClipHandle stored = pool.store("sfx/coin.fjaudio", std::make_unique<Clip>(Clip{.samples = 48}));
    CHECK(stored == held);  // so whatever held the failed handle finds it now
    CHECK(pool.state(held) == LoadState::Ready);
    REQUIRE(pool.get(held) != nullptr);
    CHECK(pool.get(held)->samples == 48);
    CHECK(pool.ready("sfx/coin.fjaudio") == held);
    CHECK(pool.slots.size() == 1);
}

TEST_CASE("storing under a new path makes a Ready slot", "[resources][pool]") {
    ClipPool pool;
    const ClipHandle first = pool.store("a.fjaudio", std::make_unique<Clip>());
    const ClipHandle second = pool.store("b.fjaudio", std::make_unique<Clip>());

    CHECK(first != second);
    CHECK(pool.ready("a.fjaudio") == first);
    CHECK(pool.ready("b.fjaudio") == second);
    CHECK_FALSE(pool.find("c.fjaudio").valid());
}

TEST_CASE("a reserved slot is not ready until it is filled", "[resources][pool]") {
    ClipPool pool;
    const ClipHandle loading = pool.reserve("mesh.fjmesh");
    CHECK(pool.state(loading) == LoadState::Loading);
    CHECK_FALSE(pool.ready("mesh.fjmesh").valid());

    pool.emplace(loading, std::make_unique<Clip>());
    CHECK(pool.ready("mesh.fjmesh") == loading);
}

TEST_CASE("a handle the pool did not give out finds nothing", "[resources][pool]") {
    ClipPool pool;
    const ClipHandle held = pool.insert("a.fjaudio", std::make_unique<Clip>());

    const ClipHandle other_life = ClipHandle::make(held.index(), held.generation() + 1);
    CHECK(pool.get(other_life) == nullptr);
    CHECK(pool.state(other_life) == LoadState::Failed);
    CHECK(pool.get(ClipHandle::make(7, 1)) == nullptr);
    CHECK(pool.get(ClipHandle{}) == nullptr);
}
