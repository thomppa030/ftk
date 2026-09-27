#include "scene/removal_epoch.hpp"
#include "scene/scene.hpp"
#include "scripting/fjell/component.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>

using namespace fjell;

namespace {

struct Health {
    int hp{0};
};

struct Probe : ScriptComponent {};

uint32_t add_node_with_id(Scene& scene, uint32_t id) {
    auto node = std::make_unique<Node3D>();
    node->id = id;
    return scene.add_node(std::move(node));
}

} // namespace

TEST_CASE("a node keeps its generation until it is destroyed", "[scene][lifetime]") {
    Scene scene;
    const uint32_t id = scene.add_node(std::make_unique<Node3D>());
    const uint32_t generation = scene.generation_of(id);
    CHECK(generation != 0);

    // Taken out and put back, as undo does: the same node, found again.
    auto out = scene.extract(id);
    CHECK(scene.generation_of(id) == 0);
    scene.restore(std::move(out), 0);
    CHECK(scene.generation_of(id) == generation);

    scene.queue_destroy(id);
    scene.flush_destroyed();
    CHECK(scene.generation_of(id) == 0);
}

TEST_CASE("a node that takes an old id, or copies a node, has a generation of its own",
          "[scene][lifetime]") {
    Scene scene;
    add_node_with_id(scene, 5);
    const uint32_t first = scene.generation_of(5);

    Scene copies;
    const uint32_t copy_id = copies.add_node(scene.find(5)->clone());
    CHECK(copies.generation_of(copy_id) != first);

    // After a clear, as when a scene is loaded again, the id is free to reuse.
    scene.clear();
    add_node_with_id(scene, 5);
    CHECK(scene.generation_of(5) != 0);
    CHECK(scene.generation_of(5) != first);
}

TEST_CASE("the removal epoch moves when something goes away, not when something arrives",
          "[scene][lifetime]") {
    Scene scene;
    uint64_t epoch = removal_epoch();

    const uint32_t id = scene.add_node(std::make_unique<Node3D>());
    Node& node = *scene.find(id);
    node.components.add<Health>();
    ScriptEntry entry;
    entry.instance = ScriptInstance::make<Probe>(node);
    CHECK(removal_epoch() == epoch);

    node.components.remove<Health>();
    CHECK(removal_epoch() > epoch);
    epoch = removal_epoch();

    entry.instance.reset();
    CHECK(removal_epoch() > epoch);
    epoch = removal_epoch();

    auto out = scene.extract(id);
    CHECK(removal_epoch() > epoch);
    epoch = removal_epoch();

    // Putting a node back brings nothing's address into question.
    scene.restore(std::move(out), 0);
    CHECK(removal_epoch() == epoch);

    scene.queue_destroy(id);
    scene.flush_destroyed();
    CHECK(removal_epoch() > epoch);
    epoch = removal_epoch();

    scene.add_node(std::make_unique<Node3D>());
    CHECK(removal_epoch() == epoch);
    scene.clear();
    CHECK(removal_epoch() > epoch);
}

TEST_CASE("a reference finds its node's component only in that node's life", "[scene][lifetime]") {
    Scene scene;
    const uint32_t id = scene.add_node(std::make_unique<Node3D>());
    Node& node = *scene.find(id);
    Health& health = node.components.add<Health>();
    ScriptEntry entry;
    entry.instance = ScriptInstance::make<Probe>(node);
    node.scripts.push_back(std::move(entry));
    const ScriptComponent* probe = node.scripts.back().instance.get();
    const uint32_t generation = scene.generation_of(id);
    const ComponentTypeId health_type = component_type_id<Health>();
    const ComponentTypeId probe_type = component_type_id<Probe>();

    CHECK(scene.resolve(id, generation, health_type) == &health);
    CHECK(scene.resolve(id, generation, probe_type) == probe);
    // Another life of the id, and a type the node does not carry.
    CHECK(scene.resolve(id, generation + 1, health_type) == nullptr);
    CHECK(scene.resolve(id, generation, component_type_id<int>()) == nullptr);

    // Out of the scene, it is gone; put back by undo, it is found again.
    auto out = scene.extract(id);
    CHECK(scene.resolve(id, generation, health_type) == nullptr);
    scene.restore(std::move(out), 0);
    CHECK(scene.resolve(id, generation, health_type) == scene.find(id)->components.get<Health>());

    scene.find(id)->components.remove<Health>();
    CHECK(scene.resolve(id, generation, health_type) == nullptr);

    scene.queue_destroy(id);
    scene.flush_destroyed();
    CHECK(scene.resolve(id, generation, probe_type) == nullptr);
}
