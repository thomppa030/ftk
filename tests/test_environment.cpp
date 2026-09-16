#include "environment/environment_resolver.hpp"
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
