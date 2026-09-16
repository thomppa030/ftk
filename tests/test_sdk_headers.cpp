// The script SDK headers are shipped to game projects and never compiled by
// the engine itself, which includes the engine's own component types instead.
// This translation unit sees them the way a game module does, so a broken
// header fails here rather than in a project's build.
#include <catch2/catch_test_macros.hpp>

#include "fjell/registry.hpp"
#include "fjell/environment_api.hpp"

TEST_CASE("The SDK publishes a layout for every mirrored engine component", "[sdk]") {
    constexpr size_t count =
        sizeof(fjell::ENGINE_COMPONENT_LAYOUTS) / sizeof(fjell::ENGINE_COMPONENT_LAYOUTS[0]);
    STATIC_REQUIRE(count == 5);
    for (const auto& layout : fjell::ENGINE_COMPONENT_LAYOUTS) {
        REQUIRE(layout.type_name != nullptr);
        REQUIRE(layout.size > 0);
    }
    STATIC_REQUIRE(sizeof(fjell::CharacterComponent) > sizeof(fjell::CollisionFilter));
}
