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

TEST_CASE("adding a type a node already has gives back the one it has", "[components]") {
    g_alive = 0;
    {
        ComponentStorage s;
        A& first = s.add<A>(1);
        s.add<B>(5);
        A& again = s.add<A>(2);

        CHECK(&again == &first);
        CHECK(first.value == 1);   // the arguments of the second add are not used
        CHECK(s.count() == 2);
        CHECK(g_alive == 2);
    }
    CHECK(g_alive == 0);
}

TEST_CASE("index_of gives a component's position, count() for none", "[components]") {
    ComponentStorage s;
    s.add<A>(1);
    s.add<B>(2);
    CHECK(s.index_of(component_type_id<A>()) == 0);
    CHECK(s.index_of(component_type_id<B>()) == 1);
    CHECK(s.index_of(component_type_id<C>()) == s.count());
}

TEST_CASE("clone_one copies one component into another storage", "[components]") {
    g_alive = 0;
    {
        ComponentStorage s;
        s.add<A>(1);
        s.add<B>(2);
        ComponentStorage copy;
        s.clone_one(component_type_id<B>(), s, copy);
        REQUIRE(copy.count() == 1);
        REQUIRE(copy.get<B>() != nullptr);
        CHECK(copy.get<B>() != s.get<B>());
        CHECK(copy.get<B>()->value == 2);

        s.clone_one(component_type_id<C>(), s, copy);
        CHECK(copy.count() == 1);
    }
    CHECK(g_alive == 0);
}

TEST_CASE("restore at a position takes a component from another storage", "[components]") {
    g_alive = 0;
    {
        ComponentStorage s;
        s.add<A>(1);
        s.add<B>(2);
        s.add<C>(3);
        ComponentStorage other;
        other.add<B>(20);

        // Swap B for other's B, keeping its place between A and C.
        const size_t at = s.index_of(component_type_id<B>());
        { auto old = s.extract(component_type_id<B>()); }
        s.restore(other.extract(component_type_id<B>()), at);

        CHECK(order(s) == std::vector{component_type_id<A>(), component_type_id<B>(),
                                      component_type_id<C>()});
        CHECK(s.get<B>()->value == 20);
        CHECK(other.empty());
        CHECK(g_alive == 3);
    }
    CHECK(g_alive == 0);
}

TEST_CASE("a component keeps its address while others come and go", "[components]") {
    g_alive = 0;
    {
        ComponentStorage s;
        s.add<A>(1);
        B& b = s.add<B>(2);
        s.add<C>(3);

        s.remove<A>();
        { auto gone = s.extract(component_type_id<C>()); }
        s.add<A>(4);
        s.add<C>(5);

        CHECK(s.get<B>() == &b);
        CHECK(b.value == 2);
        CHECK(g_alive == 3);
    }
    CHECK(g_alive == 0);
}

TEST_CASE("each component type has one descriptor", "[components]") {
    const fjell::ComponentType& a = fjell::component_type<A>();
    CHECK(&a == &fjell::component_type<A>());
    CHECK(&a != &fjell::component_type<B>());
    CHECK(a.id == component_type_id<A>());
    CHECK(a.size == sizeof(A));
    CHECK(a.align == alignof(A));
}
