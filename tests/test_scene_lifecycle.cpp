#include "scene/scene.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

using namespace fjell;

namespace {

[[nodiscard]] bool queued(const Scene& scene, uint32_t id) {
    return std::ranges::find(scene.pending_destroy(), id) != scene.pending_destroy().end();
}

} // namespace

TEST_CASE("Destroying a node destroys everything under it", "[scene_lifecycle]") {
    Scene scene;
    const uint32_t player = scene.add_empty({}, "Player");
    const uint32_t camera = scene.add_empty({}, "Camera");
    const uint32_t lens = scene.add_empty({}, "Lens");
    const uint32_t bystander = scene.add_empty({}, "Bystander");
    scene.reparent(camera, static_cast<int32_t>(player));
    scene.reparent(lens, static_cast<int32_t>(camera));

    scene.queue_destroy(player);
    // Queued before the flush, so whatever ends the scripts on doomed nodes
    // sees the children among them.
    CHECK(queued(scene, player));
    CHECK(queued(scene, camera));
    CHECK(queued(scene, lens));
    CHECK_FALSE(queued(scene, bystander));

    scene.flush_destroyed();
    CHECK(scene.find(player) == nullptr);
    CHECK(scene.find(camera) == nullptr);
    CHECK(scene.find(lens) == nullptr);
    CHECK(scene.find(bystander) != nullptr);
}

TEST_CASE("A node destroyed twice in one frame is queued once", "[scene_lifecycle]") {
    Scene scene;
    const uint32_t parent = scene.add_empty({}, "Parent");
    const uint32_t child = scene.add_empty({}, "Child");
    scene.reparent(child, static_cast<int32_t>(parent));

    scene.queue_destroy(child);
    scene.queue_destroy(parent);
    scene.queue_destroy(parent);
    CHECK(scene.pending_destroy().size() == 2);
}

TEST_CASE("While tracked, every node that joins is remembered once", "[scene_lifecycle]") {
    Scene scene;
    (void)scene.add_empty({}, "Before");
    scene.track_added(true);
    CHECK_FALSE(scene.has_added());

    // Added directly (an input handler, a collision callback)...
    const uint32_t direct = scene.add_empty({}, "Projectile");

    // ...and deferred while something walks the scene, then flushed in.
    uint32_t deferred = 0;
    {
        Scene::TickLock lock(scene);
        deferred = scene.add_empty({}, "Spawned in tick");
        // Not in the scene yet, so not joined yet.
        CHECK(scene.take_added() == std::vector<uint32_t>{direct});
    }
    CHECK_FALSE(scene.has_added());
    (void)scene.flush_spawned();
    CHECK(scene.take_added() == std::vector<uint32_t>{deferred});
    CHECK_FALSE(scene.has_added());
}

TEST_CASE("Untracked, nothing is remembered", "[scene_lifecycle]") {
    // The editor adds nodes all the time; outside play nobody reads the list.
    Scene scene;
    (void)scene.add_empty({}, "Editor node");
    CHECK_FALSE(scene.has_added());

    scene.track_added(true);
    (void)scene.add_empty({}, "Joined");
    scene.track_added(false);
    CHECK_FALSE(scene.has_added());
}

TEST_CASE("A node spawned during a tick is found by name at once", "[scene_lifecycle]") {
    // A script that spawns something uses it straight away: its children by
    // name included, although it joins the scene only after the tick.
    Scene scene;
    const uint32_t rig = scene.add_empty({}, "Rig");
    Scene::TickLock lock(scene);
    const uint32_t spawned = scene.add_empty({}, "Spawned");
    const uint32_t camera = scene.add_empty({}, "Camera");
    scene.find(camera)->parent_id = static_cast<int32_t>(spawned);

    const Node* by_name = scene.find_by_name("Spawned");
    REQUIRE(by_name != nullptr);
    CHECK(by_name->id == spawned);
    const Node* child = scene.find_child(spawned, "Camera");
    REQUIRE(child != nullptr);
    CHECK(child->id == camera);
    CHECK(scene.find_child(rig, "Camera") == nullptr);  // a child of another node
    CHECK(scene.find_by_name("Rig")->id == rig);
    CHECK(scene.find_by_name("Nobody") == nullptr);
}
