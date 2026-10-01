#include "ftk/base/handle.hpp"
#include "ftk/gpu/owned.hpp"
#include "ftk/gpu/release_queue.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace fjell;
using namespace fjell::gpu;

namespace {

// Something whose destruction a test can see.
struct Tracked {
    std::vector<std::string>* log;
    std::string name;
    Tracked(std::vector<std::string>& l, std::string n) : log(&l), name(std::move(n)) {}
    Tracked(Tracked&& other) noexcept : log(std::exchange(other.log, nullptr)), name(std::move(other.name)) {}
    Tracked(const Tracked&) = delete;
    Tracked& operator=(const Tracked&) = delete;
    Tracked& operator=(Tracked&&) = delete;
    ~Tracked() {
        if (log != nullptr) log->push_back(name);
    }
};

struct ThingTag;
using Thing = Handle<ThingTag>;

// Stands in for the device: records what it is handed back.
struct FakeOwner {
    std::vector<uint32_t> released;
};

void release(FakeOwner& owner, Thing thing) { owner.released.push_back(thing.id); }

using OwnedThing = Owned<Thing, FakeOwner>;

} // namespace

TEST_CASE("a retired object lives until the timeline reaches its value", "[gpu][release]") {
    std::vector<std::string> destroyed;
    ReleaseQueue queue;
    queue.retire(5, Tracked(destroyed, "buffer"));
    CHECK(queue.pending() == 1);

    queue.collect(4);
    CHECK(destroyed.empty());

    queue.collect(5);
    CHECK(destroyed == std::vector<std::string>{"buffer"});
    CHECK(queue.pending() == 0);
}

TEST_CASE("the release queue destroys in the order things were retired", "[gpu][release]") {
    std::vector<std::string> destroyed;
    ReleaseQueue queue;
    queue.retire(1, Tracked(destroyed, "a"));
    queue.retire(2, Tracked(destroyed, "b"));
    queue.retire(2, Tracked(destroyed, "c"));
    queue.retire(3, Tracked(destroyed, "d"));

    queue.collect(2);
    CHECK(destroyed == std::vector<std::string>{"a", "b", "c"});
    queue.collect(10);
    CHECK(destroyed == std::vector<std::string>{"a", "b", "c", "d"});
}

TEST_CASE("what a destructor retires waits for its own value", "[gpu][release]") {
    std::vector<std::string> ran;
    ReleaseQueue queue;
    queue.defer(1, [&] {
        ran.push_back("outer");
        queue.defer(4, [&] { ran.push_back("inner"); });
    });

    queue.collect(3);
    CHECK(ran == std::vector<std::string>{"outer"});
    queue.collect(4);
    CHECK(ran == std::vector<std::string>{"outer", "inner"});
}

TEST_CASE("flushing the release queue runs everything", "[gpu][release]") {
    std::vector<std::string> destroyed;
    ReleaseQueue queue;
    queue.retire(100, Tracked(destroyed, "late"));
    queue.flush();
    CHECK(destroyed == std::vector<std::string>{"late"});
    CHECK(queue.pending() == 0);
}

TEST_CASE("an owned handle goes back to its owner once", "[gpu][owned]") {
    FakeOwner owner;
    {
        OwnedThing thing(owner, Thing::make(3, 1));
        CHECK(thing);
        CHECK(thing.get() == Thing::make(3, 1));
        const Thing as_handle = thing;
        CHECK(as_handle == Thing::make(3, 1));
    }
    CHECK(owner.released == std::vector<uint32_t>{Thing::make(3, 1).id});
}

TEST_CASE("moving an owned handle hands it on without releasing it", "[gpu][owned]") {
    FakeOwner owner;
    OwnedThing first(owner, Thing::make(1, 1));
    OwnedThing second = std::move(first);
    CHECK_FALSE(first);
    CHECK(second);
    CHECK(owner.released.empty());
    second.reset();
    CHECK(owner.released == std::vector<uint32_t>{Thing::make(1, 1).id});
}

TEST_CASE("assigning over an owned handle releases the old one", "[gpu][owned]") {
    FakeOwner owner;
    OwnedThing held(owner, Thing::make(1, 1));
    held = OwnedThing(owner, Thing::make(2, 1));
    CHECK(owner.released == std::vector<uint32_t>{Thing::make(1, 1).id});
    CHECK(held.get() == Thing::make(2, 1));
}

TEST_CASE("an empty owned handle releases nothing", "[gpu][owned]") {
    FakeOwner owner;
    { OwnedThing nothing; }
    CHECK(owner.released.empty());
}
