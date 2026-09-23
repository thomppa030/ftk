#include "core/commands.hpp"
#include "scene/scene.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace fjell;

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
