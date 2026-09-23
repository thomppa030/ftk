#include "core/commands.hpp"
#include "scene/scene.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace fjell;
using Catch::Approx;

namespace {

// The scene's objects by name, in order.
std::string order(const Scene& scene) {
    std::string out;
    for (const auto& obj : scene.objects()) {
        if (!out.empty()) out += ' ';
        out += obj->name;
    }
    return out;
}

} // namespace

TEST_CASE("Removing a node closes the gap, and undo puts it back where it was", "[scene]") {
    Scene scene;
    scene.add_empty({}, "A");
    const uint32_t b = scene.add_empty({}, "B");
    scene.add_empty({}, "C");
    scene.add_empty({}, "D");

    uint32_t selected = 0;
    RemoveObjectCommand remove(scene, b, &selected);
    remove.execute();
    // Not the last node swapped into the gap.
    CHECK(order(scene) == "A C D");
    remove.undo();
    CHECK(order(scene) == "A B C D");
}

TEST_CASE("A node moves in front of a sibling, or last, keeping its place in the world", "[scene]") {
    Scene scene;
    const uint32_t folder = scene.add_folder("Folder");
    const uint32_t a = scene.add_empty({}, "A");
    Transform at;
    at.position = {3.0f, 0.0f, 0.0f};
    const uint32_t b = scene.add_empty(at, "B");
    scene.compute_world_matrices();

    scene.move(b, -1, a);
    CHECK(order(scene) == "Folder B A");
    scene.move(b, -1, 0);
    CHECK(order(scene) == "Folder A B");
    scene.move(a, static_cast<int32_t>(folder), 0);
    CHECK(scene.find(a)->parent_id == static_cast<int32_t>(folder));
    CHECK(scene.find_spatial(b)->transform.position.x == Approx(3.0f));
}

TEST_CASE("Moving several nodes is undone into exactly the order they came from", "[scene][undo]") {
    Scene scene;
    const uint32_t a = scene.add_empty({}, "A");
    scene.add_empty({}, "B");
    const uint32_t c = scene.add_empty({}, "C");
    scene.add_empty({}, "D");
    const uint32_t e = scene.add_empty({}, "E");
    scene.compute_world_matrices();

    // A and E dragged in front of C: they keep their own order.
    MoveNodesCommand move(scene, {a, e}, -1, c, "Move 2 objects");
    move.execute();
    CHECK(order(scene) == "B A E C D");
    move.undo();
    CHECK(order(scene) == "A B C D E");
    move.execute();
    CHECK(order(scene) == "B A E C D");
}
