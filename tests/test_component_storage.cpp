#include "scene/component_storage.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using fjell::ComponentStorage;
using fjell::component_type_id;

namespace {

int g_alive = 0;

struct Tracked {
    int value{0};
    explicit Tracked(int v) : value{v} { ++g_alive; }
    Tracked(const Tracked& o) : value{o.value} { ++g_alive; }
    Tracked& operator=(const Tracked&) = default;
    ~Tracked() { --g_alive; }
};
struct A : Tracked { using Tracked::Tracked; };
struct B : Tracked { using Tracked::Tracked; };
struct C : Tracked { using Tracked::Tracked; };

// The order for_each visits, as type ids.
std::vector<fjell::ComponentTypeId> order(const ComponentStorage& s) {
    std::vector<fjell::ComponentTypeId> ids;
    s.for_each([&](fjell::ComponentTypeId id, const void*) { ids.push_back(id); });
    return ids;
}

} // namespace

TEST_CASE("extract takes a component out without destroying it", "[components]") {
    g_alive = 0;
    {
        ComponentStorage s;
        B& b = s.add<B>(7);
        s.add<A>(1);

        auto detached = s.extract(component_type_id<B>());
        REQUIRE(detached);
        CHECK(detached.type_id() == component_type_id<B>());
        CHECK(s.get<B>() == nullptr);
        CHECK(g_alive == 2);

        s.restore(std::move(detached));
        CHECK_FALSE(detached);
        REQUIRE(s.get<B>() == &b);
        CHECK(s.get<B>()->value == 7);
    }
    CHECK(g_alive == 0);
}

TEST_CASE("restore puts a component back where it was", "[components]") {
    ComponentStorage s;
    s.add<A>(1);
    s.add<B>(2);
    s.add<C>(3);
    const auto before = order(s);

    auto detached = s.extract(component_type_id<B>());
    CHECK(order(s) == std::vector{component_type_id<A>(), component_type_id<C>()});

    s.restore(std::move(detached));
    CHECK(order(s) == before);
}

TEST_CASE("a detached component that is never restored is destroyed with it", "[components]") {
    g_alive = 0;
    ComponentStorage s;
    s.add<A>(1);
    {
        auto detached = s.extract(component_type_id<A>());
        CHECK(g_alive == 1);
    }
    CHECK(g_alive == 0);
    CHECK(s.empty());
}

TEST_CASE("extract of a missing type is empty and changes nothing", "[components]") {
    ComponentStorage s;
    s.add<A>(1);
    auto detached = s.extract(component_type_id<B>());
    CHECK_FALSE(detached);
    s.restore(std::move(detached));
    CHECK(s.count() == 1);
}

TEST_CASE("restore leaves a component of the same type added since in place", "[components]") {
    g_alive = 0;
    {
        ComponentStorage s;
        s.add<A>(1);
        auto detached = s.extract(component_type_id<A>());
        A& replacement = s.add<A>(2);

        s.restore(std::move(detached));
        CHECK(s.count() == 1);
        CHECK(s.get<A>() == &replacement);
        CHECK(detached);   // still owns the old one
        CHECK(g_alive == 2);
    }
    CHECK(g_alive == 0);
}
