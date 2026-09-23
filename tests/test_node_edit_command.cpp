#include "core/commands.hpp"
#include "scene/camera_node.hpp"
#include "scene/light_node.hpp"
#include "scene/node_serializer.hpp"
#include "scene/scene.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <memory>

using namespace fjell;
using Catch::Approx;

namespace {

template <typename T>
T& add(Scene& scene, uint32_t& id) {
    id = scene.add_node(std::make_unique<T>());
    return *dynamic_cast<T*>(scene.find(id));
}

} // namespace

TEST_CASE("A node's own settings come back from what they were written as", "[scene][undo]") {
    PointLightNode light;
    light.color = {0.2f, 0.4f, 0.8f};
    light.intensity = 7.0f;
    light.cast_shadows = true;
    const nlohmann::json written = serialize_node_type(light);

    PointLightNode other;
    REQUIRE(apply_node_type(other, written));
    CHECK(other.color.r == Approx(0.2f).margin(1e-4));
    CHECK(other.color.b == Approx(0.8f).margin(1e-4));
    CHECK(other.intensity == 7.0f);
    CHECK(other.cast_shadows);
}

TEST_CASE("Applying settings replaces lists and clears paths the snapshot didn't have",
          "[scene][undo]") {
    WorldEnvironmentNode before;
    const nlohmann::json empty = serialize_node_type(before);

    WorldEnvironmentNode env;
    env.hdri_path = "sky.hdr";
    env.sky_shader = "shaders/gradient_sky.fjsl";
    env.sky_uniforms.push_back({.name = "haze", .count = 1, .values = {0.5f}});
    REQUIRE(apply_node_type(env, empty));
    CHECK(env.hdri_path.empty());
    CHECK(env.sky_shader.empty());
    CHECK(env.sky_uniforms.empty());

    VolumetricFogNode fog;
    fog.params.push_back({.name = "density", .count = 1, .values = {0.1f}});
    const nlohmann::json one_param = serialize_node_type(fog);
    REQUIRE(apply_node_type(fog, one_param));
    CHECK(fog.params.size() == 1);
}

TEST_CASE("A node with no type of its own has no settings to apply", "[scene][undo]") {
    Node3D plain;
    CHECK(serialize_node_type(plain).empty());
    CHECK_FALSE(apply_node_type(plain, nlohmann::json::object()));
}

TEST_CASE("An edit of a camera's settings undoes and redoes", "[scene][undo]") {
    Scene scene;
    uint32_t id = 0;
    CameraNode& camera = add<CameraNode>(scene, id);
    const nlohmann::json before = serialize_node_type(camera);
    camera.fov = 90.0f;
    camera.projection = CameraProjection::orthographic;
    const nlohmann::json after = serialize_node_type(camera);

    NodeEditCommand edit{scene, id, before, after, "Edit Camera"};
    edit.execute();
    CHECK(camera.fov == 90.0f);
    edit.undo();
    CHECK(camera.fov == 45.0f);
    CHECK(camera.projection == CameraProjection::perspective);
    edit.execute();
    CHECK(camera.fov == 90.0f);
    CHECK(camera.projection == CameraProjection::orthographic);
}
