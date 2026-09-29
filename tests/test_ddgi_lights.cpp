// DDGI's local lights at a probe hit (fjell_ddgi_lights.glsl), compiled as
// C++: what one light gives a point, and the pick that lets a hit take one
// light for all of them. See glsl_shim.hpp.

#include "renderer/uniform_types.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <vector>

#include "glsl_shim.hpp"

#ifdef _MSC_VER
#pragma warning(push)
// A GLSL literal is a double to C++, and the shader's float arithmetic
// narrows it back.
#pragma warning(disable : 4244 4305)
#endif

namespace ddgi_lights_shim {

#include "../shaders/include/fjell_gpu_types.glsl"

// The list the tracers read from their binding.
struct LightArray {
    std::vector<DdgiLight> items;
    DdgiLight operator[](uint i) const { return items.at(i); }
};
LightArray ddgi_lights;
uint ddgi_light_count = 0;

#include "../shaders/include/fjell_ddgi_lights.glsl"

} // namespace ddgi_lights_shim

#ifdef _MSC_VER
#pragma warning(pop)
#endif

using namespace ddgi_lights_shim;
using Catch::Approx;

namespace {

DdgiLight point_light(vec3 position, float radius, vec3 color, float intensity) {
    DdgiLight light{};
    light.position = position;
    light.radius = radius;
    light.color = color;
    light.intensity = intensity;
    light.is_spot = 0;
    return light;
}

DdgiLight spot_light(vec3 position, vec3 direction, float cos_inner, float cos_outer) {
    DdgiLight light = point_light(position, 20.0f, vec3(1.0f), 10.0f);
    light.direction = direction;
    light.cos_inner = cos_inner;
    light.cos_outer = cos_outer;
    light.is_spot = 1;
    return light;
}

void set_lights(std::vector<DdgiLight> lights) {
    ddgi_lights.items = std::move(lights);
    ddgi_light_count = static_cast<uint>(ddgi_lights.items.size());
}

// A point on the floor at the origin, facing up.
const vec3 FLOOR_POINT{0.0f};
const vec3 UP{0.0f, 1.0f, 0.0f};

// Evenly spread numbers in [0, 1): the average of the pick over them is its
// average over every u, up to one step.
std::vector<float> spread(int n) {
    std::vector<float> u;
    for (int k = 0; k < n; ++k) u.push_back((static_cast<float>(k) + 0.5f) / static_cast<float>(n));
    return u;
}

} // namespace

TEST_CASE("DDGI lights: a point light falls off as the raster path lights it", "[ddgi][lights]") {
    // Two metres up, tilted so the light arrives at 60 degrees from the normal.
    const vec3 position{std::sqrt(3.0f), 1.0f, 0.0f};
    const DdgiLight light = point_light(position, 5.0f, vec3(1.0f, 0.5f, 0.25f), 8.0f);
    const DdgiLightSample s = ddgi_light_at(light, FLOOR_POINT, UP);

    const float dist2 = 4.0f;
    const float NdotL = 0.5f;
    const float expected = 8.0f * NdotL / (dist2 + 0.0001f);
    CHECK(s.distance == Approx(2.0f));
    CHECK(s.incident.x == Approx(expected));
    CHECK(s.incident.y == Approx(expected * 0.5f));
    CHECK(s.incident.z == Approx(expected * 0.25f));
    CHECK(dot(s.to_light, normalize(position)) == Approx(1.0f));
}

TEST_CASE("DDGI lights: nothing past the radius or behind the surface", "[ddgi][lights]") {
    const DdgiLight near = point_light(vec3(0.0f, 2.0f, 0.0f), 1.9f, vec3(1.0f), 8.0f);
    CHECK(ddgi_light_at(near, FLOOR_POINT, UP).incident == vec3(0.0f));

    const DdgiLight below = point_light(vec3(0.0f, -2.0f, 0.0f), 5.0f, vec3(1.0f), 8.0f);
    CHECK(ddgi_light_at(below, FLOOR_POINT, UP).incident == vec3(0.0f));
}

TEST_CASE("DDGI lights: a spot lights inside its cone and fades to its edge", "[ddgi][lights]") {
    // Pointing straight down from two metres up.
    const vec3 above{0.0f, 2.0f, 0.0f};
    const vec3 down{0.0f, -1.0f, 0.0f};
    const float cos_inner = std::cos(glm::radians(20.0f));
    const float cos_outer = std::cos(glm::radians(40.0f));
    const DdgiLight spot = spot_light(above, down, cos_inner, cos_outer);
    const DdgiLight point = point_light(above, 20.0f, vec3(1.0f), 10.0f);

    // Straight below: the full light, as a point light gives it.
    CHECK(ddgi_light_at(spot, FLOOR_POINT, UP).incident.x
          == Approx(ddgi_light_at(point, FLOOR_POINT, UP).incident.x));

    // 30 degrees off its axis: between the cones, part of it.
    const vec3 between{2.0f * std::tan(glm::radians(30.0f)), 0.0f, 0.0f};
    const float part = ddgi_light_at(spot, between, UP).incident.x;
    CHECK(part > 0.0f);
    CHECK(part < ddgi_light_at(point, between, UP).incident.x);

    // 50 degrees off: outside the outer cone, nothing.
    const vec3 outside{2.0f * std::tan(glm::radians(50.0f)), 0.0f, 0.0f};
    CHECK(ddgi_light_at(spot, outside, UP).incident == vec3(0.0f));
}

TEST_CASE("DDGI lights: no light in reach picks nothing", "[ddgi][lights]") {
    set_lights({});
    DdgiLightPick none = ddgi_pick_light(FLOOR_POINT, UP, 0.3f);
    CHECK(none.index == -1);
    CHECK(none.light.incident == vec3(0.0f));

    // A light whose radius stops short of the point is not a candidate.
    set_lights({point_light(vec3(0.0f, 3.0f, 0.0f), 2.0f, vec3(1.0f), 50.0f)});
    none = ddgi_pick_light(FLOOR_POINT, UP, 0.3f);
    CHECK(none.index == -1);
}

TEST_CASE("DDGI lights: a lone light is always picked and given whole", "[ddgi][lights]") {
    const DdgiLight light = point_light(vec3(0.0f, 2.0f, 0.0f), 5.0f, vec3(1.0f, 0.8f, 0.6f), 8.0f);
    set_lights({light});
    const vec3 whole = ddgi_light_at(light, FLOOR_POINT, UP).incident;
    for (float u : {0.0f, 0.25f, 0.5f, 0.999f}) {
        const DdgiLightPick pick = ddgi_pick_light(FLOOR_POINT, UP, u);
        CHECK(pick.index == 0);
        CHECK(pick.light.incident.x == Approx(whole.x));
        CHECK(pick.light.incident.z == Approx(whole.z));
    }
}

TEST_CASE("DDGI lights: the pick averages to the sum of every light", "[ddgi][lights]") {
    // Three lights of different strength and colour, one of them out of
    // reach, one a spot that lights the point only in part.
    const std::vector<DdgiLight> lights = {
        point_light(vec3(1.0f, 2.0f, 0.0f), 6.0f, vec3(1.0f, 0.2f, 0.2f), 12.0f),
        point_light(vec3(-3.0f, 1.5f, 1.0f), 8.0f, vec3(0.2f, 0.2f, 1.0f), 30.0f),
        point_light(vec3(0.0f, 9.0f, 0.0f), 3.0f, vec3(1.0f), 100.0f),
        // 14 degrees off its axis, between its cones.
        spot_light(vec3(0.5f, 2.0f, 0.0f), vec3(0.0f, -1.0f, 0.0f),
                   std::cos(glm::radians(5.0f)), std::cos(glm::radians(30.0f))),
    };
    set_lights(lights);

    vec3 sum{0.0f};
    std::vector<float> weights;
    for (const DdgiLight& light : lights) {
        const vec3 incident = ddgi_light_at(light, FLOOR_POINT, UP).incident;
        sum += incident;
        weights.push_back(ddgi_luminance(incident));
    }
    REQUIRE(weights[2] == 0.0f);

    const std::vector<float> us = spread(20000);
    vec3 average{0.0f};
    std::vector<int> picked(lights.size(), 0);
    for (float u : us) {
        const DdgiLightPick pick = ddgi_pick_light(FLOOR_POINT, UP, u);
        REQUIRE(pick.index >= 0);
        ++picked[static_cast<size_t>(pick.index)];
        average += pick.light.incident / static_cast<float>(us.size());
    }

    CHECK(average.x == Approx(sum.x).epsilon(0.002));
    CHECK(average.y == Approx(sum.y).epsilon(0.002));
    CHECK(average.z == Approx(sum.z).epsilon(0.002));

    // Each light is picked in proportion to what it gives; the one out of
    // reach never.
    const float total = weights[0] + weights[1] + weights[3];
    for (size_t i = 0; i < lights.size(); ++i) {
        const float share = static_cast<float>(picked[i]) / static_cast<float>(us.size());
        CHECK(share == Approx(weights[i] / total).margin(0.002));
    }
}

TEST_CASE("DDGI lights: the shaders' light record is the engine's", "[ddgi][lights]") {
    STATIC_REQUIRE(sizeof(DdgiLight) == sizeof(fjell::GpuDdgiLight));
    STATIC_REQUIRE(offsetof(DdgiLight, color) == offsetof(fjell::GpuDdgiLight, color));
    STATIC_REQUIRE(offsetof(DdgiLight, direction) == offsetof(fjell::GpuDdgiLight, direction));
    STATIC_REQUIRE(offsetof(DdgiLight, cos_outer) == offsetof(fjell::GpuDdgiLight, cos_outer));
    STATIC_REQUIRE(offsetof(DdgiLight, is_spot) == offsetof(fjell::GpuDdgiLight, is_spot));
}
