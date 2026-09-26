#include "scene/component_pool.hpp"
#include "scene/component_storage.hpp"
#include "scene/scene.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <vector>

using namespace fjell;

namespace {

int g_alive = 0;

struct Counted {
    int value{0};
    explicit Counted(int v) : value{v} { ++g_alive; }
    Counted(const Counted& o) : value{o.value} { ++g_alive; }
    Counted(Counted&& o) noexcept : value{o.value} { ++g_alive; }
    Counted& operator=(const Counted&) = default;
    Counted& operator=(Counted&&) = default;
    ~Counted() { --g_alive; }
};
struct Health : Counted { using Counted::Counted; };
struct Armour : Counted { using Counted::Counted; };

// The node ids the pool's walk visits, in order.
template <typename T>
std::vector<uint32_t> walk(Scene& scene) {
    std::vector<uint32_t> ids;
    scene.each<T>([&](Node& node, T&) { ids.push_back(node.id); });
    return ids;
}

std::unique_ptr<Node3D> node_with_health(int hp) {
    auto node = std::make_unique<Node3D>();
    node->components.add<Health>(hp);
    return node;
}

} // namespace

TEST_CASE("a pooled component keeps its address while others come and go", "[components][pool]") {
    g_alive = 0;
    {
        ComponentPools pools;
        Node node_a;
        Node node_b;
        ComponentStorage& a = node_a.components;
        ComponentStorage& b = node_b.components;
        a.attach(pools, node_a);
        b.attach(pools, node_b);

        Health& kept = a.add<Health>(7);
        for (int i = 0; i < 1000; ++i) {
            b.add<Health>(i);
            b.remove<Health>();
        }
        a.add<Armour>(3);

        CHECK(a.get<Health>() == &kept);
        CHECK(kept.value == 7);
        CHECK(pools.find(component_type_id<Health>())->size() == 1);
        a.clear();
        b.clear();
    }
    CHECK(g_alive == 0);
}

TEST_CASE("a pool walks its components in the order they joined", "[components][pool]") {
    Scene scene;
    const uint32_t first = scene.add_node(node_with_health(1));
    const uint32_t second = scene.add_node(node_with_health(2));
    const uint32_t third = scene.add_node(node_with_health(3));
    CHECK(walk<Health>(scene) == std::vector{first, second, third});

    // A slot freed in the middle and reused does not move the newcomer into
    // the gap: it joined last, so it walks last.
    scene.remove(second);
    const uint32_t fourth = scene.add_node(node_with_health(4));
    CHECK(walk<Health>(scene) == std::vector{first, third, fourth});
}

TEST_CASE("the same history walks in the same order", "[components][pool]") {
    auto build = [](Scene& scene) {
        std::vector<uint32_t> ids;
        for (int i = 0; i < 40; ++i) ids.push_back(scene.add_node(node_with_health(i)));
        for (int i = 0; i < 40; i += 3) scene.remove(ids[static_cast<std::size_t>(i)]);
        for (int i = 0; i < 10; ++i) scene.add_node(node_with_health(100 + i));
    };
    Scene one;
    Scene two;
    build(one);
    build(two);

    std::vector<int> values_one;
    std::vector<int> values_two;
    one.each<Health>([&](Node&, Health& h) { values_one.push_back(h.value); });
    two.each<Health>([&](Node&, Health& h) { values_two.push_back(h.value); });
    CHECK(values_one == values_two);
    CHECK(walk<Health>(one) == walk<Health>(two));
}

TEST_CASE("a component removed during a walk is not visited afterwards", "[components][pool]") {
    Scene scene;
    const uint32_t a = scene.add_node(node_with_health(1));
    const uint32_t b = scene.add_node(node_with_health(2));
    const uint32_t c = scene.add_node(node_with_health(3));

    std::vector<uint32_t> seen;
    scene.each<Health>([&](Node& node, Health&) {
        seen.push_back(node.id);
        if (node.id == a) scene.find(b)->components.remove<Health>();
    });
    CHECK(seen == std::vector{a, c});
    CHECK(walk<Health>(scene) == std::vector{a, c});
}

TEST_CASE("a component added during a walk is first visited by the next", "[components][pool]") {
    Scene scene;
    const uint32_t a = scene.add_node(node_with_health(1));
    const uint32_t b = scene.add_node(std::make_unique<Node3D>());

    std::vector<uint32_t> seen;
    scene.each<Health>([&](Node& node, Health&) {
        seen.push_back(node.id);
        scene.find(b)->components.add<Health>(2);
    });
    CHECK(seen == std::vector{a});
    CHECK(walk<Health>(scene) == std::vector{a, b});
}

TEST_CASE("a node leaving the scene takes its components with it, unchanged", "[components][pool]") {
    g_alive = 0;
    {
        Scene scene;
        auto built = node_with_health(5);
        built->components.add<Armour>(9);
        const uint32_t id = scene.add_node(std::move(built));
        CHECK(scene.find(id)->components.attached());
        CHECK(walk<Health>(scene) == std::vector{id});

        std::unique_ptr<Node> out = scene.extract(id);
        REQUIRE(out);
        CHECK_FALSE(out->components.attached());
        CHECK(walk<Health>(scene).empty());
        CHECK(out->components.get<Health>()->value == 5);
        CHECK(out->components.get<Armour>()->value == 9);
        CHECK(g_alive == 2);

        scene.restore(std::move(out), 0);
        CHECK(walk<Health>(scene) == std::vector{id});
        CHECK(scene.find(id)->components.get<Armour>()->value == 9);
    }
    CHECK(g_alive == 0);
}

TEST_CASE("a component extracted from a node in a scene outlives the scene", "[components][pool]") {
    g_alive = 0;
    ComponentStorage::Detached kept;
    {
        Scene scene;
        const uint32_t id = scene.add_node(node_with_health(4));
        kept = scene.find(id)->components.extract(component_type_id<Health>());
        CHECK(walk<Health>(scene).empty());
    }
    REQUIRE(kept);
    CHECK(g_alive == 1);

    Scene other;
    const uint32_t id = other.add_node(std::make_unique<Node3D>());
    other.find(id)->components.restore(std::move(kept));
    CHECK(other.find(id)->components.get<Health>()->value == 4);
    CHECK(walk<Health>(other) == std::vector{id});
}

TEST_CASE("a copy of a node in a scene belongs to no scene", "[components][pool]") {
    Scene scene;
    const uint32_t id = scene.add_node(node_with_health(6));
    std::unique_ptr<Node> copy = scene.find(id)->clone();

    CHECK_FALSE(copy->components.attached());
    CHECK(copy->components.get<Health>() != scene.find(id)->components.get<Health>());
    CHECK(copy->components.get<Health>()->value == 6);
    CHECK(walk<Health>(scene) == std::vector{id});
}

TEST_CASE("moving a scene over another releases the old nodes first", "[components][pool]") {
    g_alive = 0;
    {
        Scene scene;
        scene.add_node(node_with_health(1));
        scene.add_node(node_with_health(2));

        Scene replacement;
        const uint32_t id = replacement.add_node(node_with_health(3));
        scene = std::move(replacement);

        CHECK(walk<Health>(scene) == std::vector{id});
        CHECK(g_alive == 1);

        // The moved-from scene is still usable.
        const uint32_t again = replacement.add_node(node_with_health(4));
        CHECK(walk<Health>(replacement) == std::vector{again});
    }
    CHECK(g_alive == 0);
}

TEST_CASE("a node spawned during a tick lives in the pools from the start", "[components][pool]") {
    g_alive = 0;
    {
        Scene scene;
        uint32_t spawned = 0;
        Health* health = nullptr;
        {
            Scene::TickLock lock(scene);
            spawned = scene.add_node(std::make_unique<Node3D>());
            // Handed out before the flush; it must stay where it is.
            health = &scene.find(spawned)->components.add<Health>(8);
        }
        scene.flush_spawned();
        CHECK(scene.find(spawned)->components.get<Health>() == health);
        CHECK(walk<Health>(scene) == std::vector{spawned});
    }
    CHECK(g_alive == 0);
}

TEST_CASE("a pool can be walked as a range, with the same rules", "[components][pool]") {
    Scene scene;
    const uint32_t a = scene.add_node(node_with_health(1));
    const uint32_t b = scene.add_node(node_with_health(2));
    const uint32_t c = scene.add_node(node_with_health(3));

    std::vector<uint32_t> seen;
    uint32_t joined = 0;
    for (auto [node, health] : scene.view<Health>()) {
        seen.push_back(node->id);
        if (health->value == 1) {
            scene.remove(b);
            joined = scene.add_node(node_with_health(4));
        }
    }
    CHECK(seen == std::vector{a, c});

    const Scene& view_only = scene;
    std::vector<int> values;
    for (auto [node, health] : view_only.view<Health>()) values.push_back(health->value);
    CHECK(values == std::vector{1, 3, 4});
    CHECK(walk<Health>(scene) == std::vector{a, c, joined});

    Scene empty;
    CHECK(empty.view<Health>().begin() == empty.view<Health>().end());
}

TEST_CASE("the first holder of a type is the node that has had one longest", "[components][pool]") {
    Scene scene;
    CHECK(scene.first_holder(component_type_id<Health>()) == nullptr);

    const uint32_t a = scene.add_node(node_with_health(1));
    const uint32_t b = scene.add_node(node_with_health(2));
    CHECK(scene.first_holder(component_type_id<Health>())->id == a);

    // A component that left during a walk is still in the pool's books until
    // the walk ends, and is passed over.
    scene.each<Health>([&](Node& node, Health&) {
        if (node.id == a) {
            node.components.remove<Health>();
            CHECK(scene.first_holder(component_type_id<Health>())->id == b);
        }
    });
    CHECK(scene.first_holder(component_type_id<Health>())->id == b);

    scene.remove(b);
    CHECK(scene.first_holder(component_type_id<Health>()) == nullptr);

    // A node spawned during a tick holds its components before the flush.
    Scene::TickLock lock(scene);
    const uint32_t spawned = scene.add_node(node_with_health(3));
    CHECK(scene.first_holder(component_type_id<Health>())->id == spawned);
}
