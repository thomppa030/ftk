#include "scene/camera_node.hpp"
#include "scene/game_ui_node.hpp"
#include "scene/light_node.hpp"
#include "scene/scene.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <vector>

using namespace fjell;

namespace {

template <typename T>
std::vector<uint32_t> walk(const Scene& scene) {
    std::vector<uint32_t> ids;
    scene.each_node<T>([&](const T& node) { ids.push_back(node.id); });
    return ids;
}

template <typename T>
uint32_t add(Scene& scene) {
    return scene.add_node(std::make_unique<T>());
}

template <typename T>
void check_kind() {
    const T node;
    const Node& as_node = node;
    CHECK(as_node.kind() == T::KIND);
    CHECK(T::KIND != NodeKind::None);
}

} // namespace

TEST_CASE("every node kind reports its own kind", "[scene][kinds]") {
    check_kind<CameraNode>();
    check_kind<DirectionalLightNode>();
    check_kind<PointLightNode>();
    check_kind<SpotLightNode>();
    check_kind<VolumetricFogNode>();
    check_kind<FogVolumeNode>();
    check_kind<PostProcessVolumeNode>();
    check_kind<WorldEnvironmentNode>();
    check_kind<GameUiNode>();
    CHECK(Node3D{}.kind() == NodeKind::None);
    CHECK(Node{}.kind() == NodeKind::None);
}

TEST_CASE("a kind is walked in the order its nodes joined", "[scene][kinds]") {
    Scene scene;
    const uint32_t a = add<PointLightNode>(scene);
    add<SpotLightNode>(scene);
    scene.add_empty({}, "plain");
    const uint32_t b = add<PointLightNode>(scene);
    const uint32_t c = add<PointLightNode>(scene);

    CHECK(walk<PointLightNode>(scene) == std::vector{a, b, c});
    CHECK(walk<SpotLightNode>(scene).size() == 1);
    CHECK(walk<CameraNode>(scene).empty());
}

TEST_CASE("a node leaving during a walk is not visited afterwards", "[scene][kinds]") {
    Scene scene;
    const uint32_t a = add<PointLightNode>(scene);
    const uint32_t b = add<PointLightNode>(scene);
    const uint32_t c = add<PointLightNode>(scene);

    std::vector<uint32_t> seen;
    scene.each_node<PointLightNode>([&](PointLightNode& light) {
        seen.push_back(light.id);
        if (light.id == a) scene.remove(b);
    });
    CHECK(seen == std::vector{a, c});
    CHECK(walk<PointLightNode>(scene) == std::vector{a, c});
}

TEST_CASE("a node joining during a walk is first visited by the next", "[scene][kinds]") {
    Scene scene;
    const uint32_t a = add<PointLightNode>(scene);

    std::vector<uint32_t> seen;
    uint32_t joined = 0;
    scene.each_node<PointLightNode>([&](PointLightNode& light) {
        seen.push_back(light.id);
        joined = add<PointLightNode>(scene);
    });
    CHECK(seen == std::vector{a});
    CHECK(walk<PointLightNode>(scene) == std::vector{a, joined});
}

TEST_CASE("a node spawned during a tick joins its kind at the flush", "[scene][kinds]") {
    Scene scene;
    uint32_t spawned = 0;
    {
        Scene::TickLock lock(scene);
        spawned = add<FogVolumeNode>(scene);
        CHECK(walk<FogVolumeNode>(scene).empty());
    }
    scene.flush_spawned();
    CHECK(walk<FogVolumeNode>(scene) == std::vector{spawned});
}

TEST_CASE("a node put back by undo rejoins at the end of its kind", "[scene][kinds]") {
    Scene scene;
    const uint32_t a = add<SpotLightNode>(scene);
    const uint32_t b = add<SpotLightNode>(scene);

    std::unique_ptr<Node> out = scene.extract(a);
    CHECK(walk<SpotLightNode>(scene) == std::vector{b});
    scene.restore(std::move(out), 0);
    CHECK(walk<SpotLightNode>(scene) == std::vector{b, a});
}

TEST_CASE("a destroyed node leaves its kind", "[scene][kinds]") {
    Scene scene;
    const uint32_t a = add<CameraNode>(scene);
    const uint32_t b = add<CameraNode>(scene);
    CHECK(scene.first_node<CameraNode>()->id == a);

    scene.queue_destroy(a);
    scene.flush_destroyed();
    CHECK(walk<CameraNode>(scene) == std::vector{b});
    REQUIRE(scene.first_node<CameraNode>() != nullptr);
    CHECK(scene.first_node<CameraNode>()->id == b);

    scene.clear();
    CHECK(scene.first_node<CameraNode>() == nullptr);
}

TEST_CASE("a const scene can be walked and asked for its first of a kind", "[scene][kinds]") {
    Scene scene;
    const uint32_t env = add<WorldEnvironmentNode>(scene);
    const Scene& view = scene;
    REQUIRE(view.first_node<WorldEnvironmentNode>() != nullptr);
    CHECK(view.first_node<WorldEnvironmentNode>()->id == env);
    CHECK(walk<WorldEnvironmentNode>(view) == std::vector{env});
}
