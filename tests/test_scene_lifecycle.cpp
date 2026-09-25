#include "scene/scene.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

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
