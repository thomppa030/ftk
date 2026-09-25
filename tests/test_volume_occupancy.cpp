#include "scene/volume_occupancy.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/gtc/matrix_transform.hpp>

// The shader half, compiled as C++ so the two can be compared directly. See
// glsl_shim.hpp.
#include "glsl_shim.hpp"

namespace volume_shim {
#include "../shaders/include/fjell_volume_shape.glsl"
} // namespace volume_shim

using namespace fjell;
using Catch::Approx;

namespace {

// A volume placed the way a scene places one: moved, turned and stretched
// unevenly, so the inverse transform is doing real work.
glm::mat4 placed_volume() {
    glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(12.0f, -3.0f, 40.0f));
    m = glm::rotate(m, glm::radians(35.0f), glm::normalize(glm::vec3(0.3f, 1.0f, 0.1f)));
    return glm::scale(m, glm::vec3(8.0f, 2.5f, 14.0f));
}

int glsl_shape(ColliderShape shape) {
    return shape == ColliderShape::Sphere ? FJELL_VOLUME_SHAPE_SPHERE : FJELL_VOLUME_SHAPE_BOX;
}

} // namespace

TEST_CASE("The CPU occupancy matches the shader's everywhere", "[volume]") {
    const glm::mat4 world = placed_volume();
    const glm::mat4 inv = glm::inverse(world);

    for (ColliderShape shape : {ColliderShape::Box, ColliderShape::Sphere}) {
        for (float falloff : {0.0f, 0.05f, 0.2f, 0.5f, 1.0f}) {
            // Local points across and past the unit shape, taken to world
            // space, so the grid straddles the edge from every side.
            for (int x = -6; x <= 6; ++x) {
                for (int y = -6; y <= 6; ++y) {
                    for (int z = -6; z <= 6; ++z) {
                        const glm::vec3 local = glm::vec3(x, y, z) * 0.22f;
                        const glm::vec3 world_pos{world * glm::vec4(local, 1.0f)};
                        const float cpu = volume_occupancy(shape, inv, world_pos, falloff);
                        const float gpu = volume_shim::volume_occupancy(glsl_shape(shape), inv,
                                                                        world_pos, falloff);
                        INFO("shape " << static_cast<int>(shape) << " falloff " << falloff
                             << " local " << local.x << ", " << local.y << ", " << local.z);
                        REQUIRE(cpu == Approx(gpu).margin(1e-5));
                    }
                }
            }
        }
    }
}

TEST_CASE("Occupancy is whole inside, nothing outside, and fades across the edge", "[volume]") {
    const glm::mat4 world = placed_volume();
    const glm::mat4 inv = glm::inverse(world);
    auto at = [&](glm::vec3 local) { return glm::vec3{world * glm::vec4(local, 1.0f)}; };

    for (ColliderShape shape : {ColliderShape::Box, ColliderShape::Sphere}) {
        REQUIRE(volume_occupancy(shape, inv, at({0.0f, 0.0f, 0.0f}), 0.2f) == Approx(1.0f));
        REQUIRE(volume_occupancy(shape, inv, at({1.5f, 0.0f, 0.0f}), 0.2f) == Approx(0.0f));
        // Halfway through a falloff of 0.2 on the x face: half in.
        REQUIRE(volume_occupancy(shape, inv, at({0.9f, 0.0f, 0.0f}), 0.2f) == Approx(0.5f));
    }
    // A corner of the box fades once, not once per axis.
    const float face = volume_occupancy(ColliderShape::Box, inv, at({0.9f, 0.0f, 0.0f}), 0.2f);
    const float corner = volume_occupancy(ColliderShape::Box, inv, at({0.9f, 0.9f, 0.9f}), 0.2f);
    REQUIRE(corner == Approx(face));
    // The shapes a volume cannot be yet read as the box, on both sides.
    REQUIRE(volume_occupancy(ColliderShape::Capsule, inv, at({0.9f, 0.9f, 0.0f}), 0.2f)
            == Approx(volume_occupancy(ColliderShape::Box, inv, at({0.9f, 0.9f, 0.0f}), 0.2f)));
}
