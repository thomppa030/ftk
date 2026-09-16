#include "environment/environment_resolver.hpp"
#include "environment/environment_ubo.hpp"
#include "environment/sun_path.hpp"
#include "scene/light_node.hpp"
#include "scene/scene.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <memory>

using namespace fjell;
using Catch::Approx;

namespace {

bool close(const glm::vec3& a, const glm::vec3& b) {
    return glm::length(a - b) < 1e-4f;
}

} // namespace

TEST_CASE("An empty scene resolves to a lit default environment", "[environment]") {
    Scene scene;
    auto env = resolve_environment(scene);

    // A default sun so a bare scene still reads, pointing up and away from
    // the authored travel direction.
    REQUIRE(env.sun.lit());
    REQUIRE(env.sun.direction.y > 0.0f);
    REQUIRE(glm::length(env.sun.direction) == Approx(1.0f));
    REQUIRE(env.sun.elevation > 0.0f);

    // No environment or fog node: sky defaults, fog off, shafts off.
    REQUIRE_FALSE(env.fog.enabled);
    REQUIRE_FALSE(env.sun_shafts.enabled);
    REQUIRE(env.sky.hdri_path.empty());
    REQUIRE_FALSE(env.sky.atmosphere_enabled);

    // The sun is the shadow caster and nothing lights from the other slot.
    REQUIRE(close(env.primary.direction, env.sun.direction));
    REQUIRE_FALSE(env.secondary.lit());
}

TEST_CASE("The first directional light becomes the sun", "[environment]") {
    Scene scene;
    auto sun = std::make_unique<DirectionalLightNode>();
    sun->direction = glm::vec3(0.0f, -1.0f, 0.0f);
    sun->color = glm::vec3(1.0f, 0.5f, 0.25f);
    sun->intensity = 3.0f;
    sun->ambient_color = glm::vec3(0.2f, 0.3f, 0.4f);
    sun->ambient_intensity = 0.75f;
    scene.add_node(std::move(sun));

    auto second = std::make_unique<DirectionalLightNode>();
    second->direction = glm::vec3(1.0f, 0.0f, 0.0f);
    second->intensity = 99.0f;
    scene.add_node(std::move(second));

    auto env = resolve_environment(scene);
    REQUIRE(close(env.sun.direction, glm::vec3(0.0f, 1.0f, 0.0f)));
    REQUIRE(close(env.sun.color, glm::vec3(1.0f, 0.5f, 0.25f)));
    REQUIRE(env.sun.intensity == Approx(3.0f));
    REQUIRE(env.sun.elevation == Approx(glm::half_pi<float>()));
    REQUIRE(close(env.ambient_color, glm::vec3(0.2f, 0.3f, 0.4f)));
    REQUIRE(env.ambient_intensity == Approx(0.75f));
    REQUIRE(env.primary.intensity == Approx(3.0f));
}

TEST_CASE("Environment and fog nodes fill their settings", "[environment]") {
    Scene scene;
    auto world = std::make_unique<WorldEnvironmentNode>();
    world->sky_top_color = glm::vec3(0.1f, 0.2f, 0.3f);
    world->ibl_intensity = 2.5f;
    world->hdri_path = "assets/sky.hdr";
    world->sky_shader = "shaders/gradient_sky.fjsl";
    world->atmosphere_enabled = true;
    world->atmosphere_mie_g = 0.5f;
    world->contact_shadows_enabled = false;
    world->contact_shadow_steps = 7;
    scene.add_node(std::move(world));

    auto fog = std::make_unique<VolumetricFogNode>();
    fog->density = 0.02f;
    fog->fog_color = glm::vec3(0.5f, 0.6f, 0.7f);
    fog->transform.position = glm::vec3(0.0f, 12.0f, 0.0f);
    fog->shafts_enabled = true;
    fog->shaft_step_count = 9;
    scene.add_node(std::move(fog));
    scene.compute_world_matrices(nullptr);

    auto env = resolve_environment(scene);
    REQUIRE(close(env.sky.top_color, glm::vec3(0.1f, 0.2f, 0.3f)));
    REQUIRE(env.sky.ibl_intensity == Approx(2.5f));
    REQUIRE(env.sky.hdri_path == "assets/sky.hdr");
    REQUIRE(env.sky.sky_shader == "shaders/gradient_sky.fjsl");
    REQUIRE(env.sky.atmosphere_enabled);
    REQUIRE(env.sky.atmosphere_mie_g == Approx(0.5f));
    REQUIRE_FALSE(env.contact_shadows.enabled);
    REQUIRE(env.contact_shadows.steps == 7);

    REQUIRE(env.fog.enabled);
    REQUIRE(env.fog.density == Approx(0.02f));
    REQUIRE(close(env.fog.color, glm::vec3(0.5f, 0.6f, 0.7f)));
    REQUIRE(env.fog.height == Approx(12.0f));
    REQUIRE(env.sun_shafts.enabled);
    REQUIRE(env.sun_shafts.step_count == 9);
}

TEST_CASE("The environment UBO packs both bodies", "[environment]") {
    EnvironmentState env;
    env.sun.direction = glm::vec3(0.0f, 1.0f, 0.0f);
    env.sun.intensity = 2.0f;
    env.sun.color = glm::vec3(1.0f, 0.9f, 0.8f);
    env.secondary.direction = glm::vec3(1.0f, 0.0f, 0.0f);
    env.secondary.intensity = 0.25f;

    auto ubo = make_environment_ubo(env);
    REQUIRE(ubo.sun_dir_intensity.y == Approx(1.0f));
    REQUIRE(ubo.sun_dir_intensity.w == Approx(2.0f));
    REQUIRE(ubo.sun_color.g == Approx(0.9f));
    REQUIRE(ubo.secondary_dir_intensity.x == Approx(1.0f));
    REQUIRE(ubo.secondary_dir_intensity.w == Approx(0.25f));
    REQUIRE(ubo.moon_dir_intensity.w == Approx(0.0f));
    // Slots nothing drives are neutral: no tint, no clouds, nothing falling.
    REQUIRE(ubo.sky_tint_star.r == Approx(1.0f));
    REQUIRE(ubo.weather0.x == Approx(0.0f));
    REQUIRE(ubo.weather0.z == Approx(0.0f));
}

TEST_CASE("The sun path puts noon overhead and sunrise in the east", "[environment][sun_path]") {
    SunPath flat{.tilt_degrees = 0.0f, .north_yaw_degrees = 0.0f, .noon_hour = 12.0f};
    REQUIRE(close(sun_direction(flat, 12.0f), glm::vec3(0.0f, 1.0f, 0.0f)));
    REQUIRE(close(sun_direction(flat, 6.0f), glm::vec3(1.0f, 0.0f, 0.0f)));
    REQUIRE(close(sun_direction(flat, 18.0f), glm::vec3(-1.0f, 0.0f, 0.0f)));
    REQUIRE(close(sun_direction(flat, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)));

    // Tilting leans the noon sun toward the south and lowers it by the tilt;
    // sunrise and sunset stay on the horizon, twelve hours apart.
    SunPath tilted{.tilt_degrees = 40.0f, .north_yaw_degrees = 0.0f, .noon_hour = 12.0f};
    const glm::vec3 noon = sun_direction(tilted, 12.0f);
    REQUIRE(elevation_of(noon) == Approx(glm::radians(50.0f)).margin(1e-4));
    REQUIRE(noon.z > 0.0f);
    REQUIRE(elevation_of(sun_direction(tilted, 6.0f)) == Approx(0.0f).margin(1e-4));
    REQUIRE(elevation_of(sun_direction(tilted, 18.0f)) == Approx(0.0f).margin(1e-4));
    REQUIRE(elevation_of(sun_direction(tilted, 9.0f)) ==
            Approx(elevation_of(sun_direction(tilted, 15.0f))).margin(1e-4));

    // Turning north swings the compass without changing the height.
    SunPath turned{.tilt_degrees = 40.0f, .north_yaw_degrees = 90.0f, .noon_hour = 12.0f};
    REQUIRE(elevation_of(sun_direction(turned, 12.0f)) == Approx(glm::radians(50.0f)).margin(1e-4));
    REQUIRE(close(sun_direction(turned, 6.0f), glm::vec3(0.0f, 0.0f, -1.0f)));

    // A later noon shifts the whole day and hours wrap.
    SunPath late{.tilt_degrees = 0.0f, .north_yaw_degrees = 0.0f, .noon_hour = 14.0f};
    REQUIRE(close(sun_direction(late, 14.0f), glm::vec3(0.0f, 1.0f, 0.0f)));
    REQUIRE(close(sun_direction(late, 26.0f), sun_direction(late, 2.0f)));
    REQUIRE(wrap_hour(25.5f) == Approx(1.5f));
    REQUIRE(wrap_hour(-1.0f) == Approx(23.0f));
}

TEST_CASE("Horizon fade and time phase follow the sun's height", "[environment][sun_path]") {
    REQUIRE(horizon_fade(glm::radians(30.0f)) == Approx(1.0f));
    REQUIRE(horizon_fade(glm::radians(-30.0f)) == Approx(0.0f));
    REQUIRE(horizon_fade(0.0f) > 0.0f);
    REQUIRE(horizon_fade(0.0f) < 1.0f);

    REQUIRE(time_phase(glm::radians(30.0f), 12.0f, 12.0f) == TimePhase::Day);
    REQUIRE(time_phase(glm::radians(-30.0f), 0.0f, 12.0f) == TimePhase::Night);
    REQUIRE(time_phase(0.0f, 6.0f, 12.0f) == TimePhase::Dawn);
    REQUIRE(time_phase(0.0f, 18.0f, 12.0f) == TimePhase::Dusk);
}

TEST_CASE("A time of day drives the sun from the hour", "[environment]") {
    Scene scene;
    auto sun = std::make_unique<DirectionalLightNode>();
    sun->direction = glm::vec3(1.0f, 0.0f, 0.0f);   // ignored while the clock runs
    sun->intensity = 2.0f;
    scene.add_node(std::move(sun));
    auto world = std::make_unique<WorldEnvironmentNode>();
    world->time_of_day_enabled = true;
    world->sun_path_tilt = 0.0f;
    world->time_of_day = 12.0f;
    auto* world_ptr = world.get();
    scene.add_node(std::move(world));

    auto noon = resolve_environment(scene);
    REQUIRE(noon.hour == Approx(12.0f));
    REQUIRE(noon.star_intensity == Approx(0.0f));
    REQUIRE(close(noon.sun.direction, glm::vec3(0.0f, 1.0f, 0.0f)));
    REQUIRE(noon.sun.intensity == Approx(2.0f));
    REQUIRE(noon.phase == TimePhase::Day);
    REQUIRE(close(noon.primary.direction, noon.sun.direction));

    world_ptr->time_of_day = 0.0f;
    auto midnight = resolve_environment(scene);
    REQUIRE(midnight.sun.elevation < 0.0f);
    REQUIRE(midnight.sun.intensity == Approx(0.0f));
    REQUIRE(midnight.phase == TimePhase::Night);

    world_ptr->time_of_day_enabled = false;
    auto authored = resolve_environment(scene);
    REQUIRE(close(authored.sun.direction, glm::vec3(-1.0f, 0.0f, 0.0f)));
}

TEST_CASE("The shadow caster hands over with hysteresis", "[environment]") {
    DirectionalBody sun;
    sun.direction = glm::vec3(0.0f, 1.0f, 0.0f);
    sun.elevation = glm::half_pi<float>();
    DirectionalBody moon = sun;

    sun.intensity = 1.0f;
    moon.intensity = 0.0f;
    REQUIRE_FALSE(moon_leads(sun, moon, false));
    REQUIRE_FALSE(moon_leads(sun, moon, true));   // an unlit moon never leads

    // Slightly brighter is not enough to take over, clearly brighter is.
    moon.intensity = 1.1f;
    REQUIRE_FALSE(moon_leads(sun, moon, false));
    moon.intensity = 1.5f;
    REQUIRE(moon_leads(sun, moon, false));

    // Once leading, the moon keeps the role until the sun clearly wins.
    moon.intensity = 0.9f;
    REQUIRE(moon_leads(sun, moon, true));
    moon.intensity = 0.5f;
    REQUIRE_FALSE(moon_leads(sun, moon, true));

    // A set sun leaves the moon in charge at any brightness.
    sun.elevation = glm::radians(-20.0f);
    moon.intensity = 0.05f;
    REQUIRE(moon_leads(sun, moon, false));
}

TEST_CASE("The moon rides the sun path behind the sun and the sky turns about its axis", "[environment]") {
    SunPath path{.tilt_degrees = 40.0f, .north_yaw_degrees = 30.0f, .noon_hour = 12.0f};

    // Rotating the noon sun about the path axis by the hour's angle lands on
    // the sun for that hour, so the star field can follow the same turn.
    const glm::vec3 axis = sun_path_axis(path);
    const glm::vec3 noon = sun_direction(path, 12.0f);
    for (float hour : {3.0f, 9.0f, 15.0f, 20.5f}) {
        const float a = sun_path_angle(path, hour);
        const glm::vec3 rotated = noon * std::cos(a) + glm::cross(axis, noon) * std::sin(a)
                                  + axis * glm::dot(axis, noon) * (1.0f - std::cos(a));
        REQUIRE(close(rotated, sun_direction(path, hour)));
    }

    REQUIRE(moon_phase_brightness(0.0f) == Approx(0.0f).margin(1e-6));
    REQUIRE(moon_phase_brightness(0.5f) == Approx(1.0f));
    REQUIRE(moon_phase_brightness(1.0f) == Approx(0.0f).margin(1e-6));

    Scene scene;
    scene.add_node(std::make_unique<DirectionalLightNode>());
    auto world = std::make_unique<WorldEnvironmentNode>();
    world->time_of_day_enabled = true;
    world->sun_path_tilt = 0.0f;
    world->time_of_day = 0.0f;         // midnight
    world->moon_offset_hours = 12.0f;
    world->moon_phase = 0.5f;
    world->moon_intensity = 0.1f;
    auto* world_ptr = world.get();
    scene.add_node(std::move(world));

    auto midnight = resolve_environment(scene);
    REQUIRE(midnight.star_intensity == Approx(1.0f));
    REQUIRE(close(midnight.moon.direction, glm::vec3(0.0f, 1.0f, 0.0f)));
    REQUIRE(midnight.moon.intensity == Approx(0.1f));
    REQUIRE_FALSE(midnight.sun.lit());
    // With the sun set, the full moon casts the shadows and the sun is the
    // dark secondary.
    REQUIRE(close(midnight.primary.direction, midnight.moon.direction));
    REQUIRE(midnight.secondary.intensity == Approx(0.0f));

    world_ptr->moon_phase = 0.0f;
    auto new_moon = resolve_environment(scene);
    REQUIRE(new_moon.moon.intensity == Approx(0.0f).margin(1e-6));
    REQUIRE(close(new_moon.primary.direction, new_moon.sun.direction));
}
