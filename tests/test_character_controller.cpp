#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "physics/physics_types.hpp"
#include "physics/physics_world.hpp"
#include "scene/scene.hpp"

#include <glm/gtc/quaternion.hpp>

using namespace fjell;
using Catch::Approx;

namespace {

constexpr float STEP_DT = 1.0f / 60.0f;
constexpr float RADIUS = 0.3f;
constexpr float HEIGHT = 1.4f;

// A scene with a physics world stepped the way the engine steps it: physics,
// then world matrices, so parented nodes see the result the renderer would.
struct CharacterWorld {
    Scene scene;
    PhysicsWorld physics;

    uint32_t add_box(const glm::vec3& position, const glm::vec3& half_extents,
                     const glm::quat& rotation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f},
                     bool trigger = false) {
        Transform t;
        t.position = position;
        t.rotation = rotation;
        uint32_t id = scene.add_empty(t, trigger ? "trigger" : "box");
        auto& collider = scene.find_spatial(id)->components.add<ColliderComponent>();
        collider.shape = ColliderShape::Box;
        collider.half_extents = half_extents;
        collider.is_trigger = trigger;
        return id;
    }

    // A 20 m square floor whose top face is at y = 0.
    uint32_t add_floor() { return add_box({0.0f, -0.5f, 0.0f}, {10.0f, 0.5f, 10.0f}); }

    uint32_t add_character(const glm::vec3& position, int32_t parent = -1) {
        Transform t;
        t.position = position;
        uint32_t id = scene.add_empty(t, "character");
        auto* node = scene.find_spatial(id);
        node->parent_id = parent;
        auto& cc = node->components.add<CharacterComponent>();
        cc.capsule_radius = RADIUS;
        cc.capsule_height = HEIGHT;
        return id;
    }

    void start() {
        scene.compute_world_matrices();
        physics.create_bodies(scene);
    }

    void step(int frames) {
        for (int i = 0; i < frames; ++i) {
            physics.step(STEP_DT, scene);
            scene.compute_world_matrices();
        }
    }

    // Steps until the character reports ground under it, or gives up.
    bool settle(uint32_t id, int max_frames = 240) {
        for (int i = 0; i < max_frames; ++i) {
            step(1);
            if (character(id).ground_state == CharacterGroundState::OnGround) {
                step(10);
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] Node3D& node(uint32_t id) { return *scene.find_spatial(id); }
    [[nodiscard]] CharacterComponent& character(uint32_t id) {
        return *node(id).components.get<CharacterComponent>();
    }
    [[nodiscard]] glm::vec3 world_position(uint32_t id) {
        return glm::vec3{node(id).world_matrix_[3]};
    }
};

} // namespace

TEST_CASE("A character falls onto the floor and reports it", "[character]") {
    CharacterWorld w;
    w.add_floor();
    auto id = w.add_character({0.0f, 2.0f, 0.0f});
    w.start();

    REQUIRE(w.settle(id));
    REQUIRE(w.character(id).ground_normal.y == Approx(1.0f).margin(0.01f));
    REQUIRE(w.character(id).linear_velocity.y == Approx(0.0f).margin(0.05f));
}

TEST_CASE("A grounded character's feet rest on the floor at the node origin", "[character]") {
    CharacterWorld w;
    w.add_floor();
    auto id = w.add_character({0.0f, 2.0f, 0.0f});
    w.start();
    REQUIRE(w.settle(id));

    // The node origin is where the mesh's feet are; a capsule centered on it
    // would leave the node a full half-capsule above the floor.
    REQUIRE(w.node(id).transform.position.y == Approx(0.0f).margin(0.05f));
}

TEST_CASE("Writing an upward velocity while grounded starts a jump", "[character]") {
    CharacterWorld w;
    w.add_floor();
    auto id = w.add_character({0.0f, 2.0f, 0.0f});
    w.start();
    REQUIRE(w.settle(id));
    float rest_y = w.node(id).transform.position.y;

    w.character(id).linear_velocity.y = 5.0f;
    w.step(1);
    REQUIRE(w.character(id).ground_state == CharacterGroundState::InAir);

    float peak = rest_y;
    for (int i = 0; i < 60; ++i) {
        w.step(1);
        peak = std::max(peak, w.node(id).transform.position.y);
    }
    // 5 m/s under 9.81 m/s² gravity peaks at about 1.27 m.
    REQUIRE(peak - rest_y > 1.0f);
}

TEST_CASE("A character on ground steeper than its slope limit slides down", "[character]") {
    CharacterWorld w;
    // A 70° ramp, well past the 50° default limit. Rotated about Z so its
    // top face tilts toward -x, and placed so the character lands on it.
    auto tilt = glm::angleAxis(glm::radians(-70.0f), glm::vec3{0.0f, 0.0f, 1.0f});
    w.add_box({0.0f, 0.0f, 0.0f}, {10.0f, 0.5f, 10.0f}, tilt);
    auto id = w.add_character({1.0f, 4.0f, 0.0f});
    w.start();

    // Let it land; it must not be reported as walkable ground.
    w.step(90);
    REQUIRE(w.character(id).ground_state == CharacterGroundState::OnSteepGround);
    float y_on_contact = w.node(id).transform.position.y;

    w.step(60);
    REQUIRE(w.node(id).transform.position.y < y_on_contact - 0.5f);
}

TEST_CASE("Low steps are climbed and tall steps block", "[character]") {
    auto walk_into_step = [](float step_height) {
        CharacterWorld w;
        w.add_floor();
        w.add_box({3.0f, step_height * 0.5f, 0.0f}, {1.0f, step_height * 0.5f, 5.0f});
        auto id = w.add_character({0.0f, 2.0f, 0.0f});
        w.start();
        REQUIRE(w.settle(id));
        float rest_y = w.node(id).transform.position.y;

        for (int i = 0; i < 120; ++i) {
            w.character(id).linear_velocity.x = 2.0f;
            w.character(id).linear_velocity.z = 0.0f;
            w.step(1);
        }
        return w.node(id).transform.position.y - rest_y;
    };

    REQUIRE(walk_into_step(0.3f) == Approx(0.3f).margin(0.05f));
    REQUIRE(walk_into_step(0.8f) == Approx(0.0f).margin(0.05f));
}

TEST_CASE("A character that stood still is still detected by trigger volumes", "[character]") {
    CharacterWorld w;
    w.add_floor();
    auto trigger = w.add_box({4.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 0.0f}, true);
    auto id = w.add_character({0.0f, 2.0f, 0.0f});
    w.start();
    REQUIRE(w.settle(id));

    // Idle long enough for a body that is allowed to sleep to do so.
    w.step(180);

    bool entered = false;
    for (int i = 0; i < 180 && !entered; ++i) {
        w.character(id).linear_velocity.x = 2.0f;
        w.character(id).linear_velocity.z = 0.0f;
        w.step(1);
        for (const auto& [node_id, event] : w.physics.trigger_enter_events()) {
            if (node_id == trigger && event.other_id == id) entered = true;
        }
    }
    REQUIRE(entered);
}

TEST_CASE("A parented character lands on the floor in world space", "[character]") {
    CharacterWorld w;
    w.add_floor();
    Transform parent_t;
    parent_t.position = {0.0f, 5.0f, 0.0f};
    auto parent = w.scene.add_empty(parent_t, "parent");
    auto id = w.add_character({0.0f, 1.0f, 0.0f}, static_cast<int32_t>(parent));
    w.start();

    REQUIRE(w.settle(id));
    REQUIRE(w.world_position(id).y == Approx(0.0f).margin(0.05f));
}

TEST_CASE("A position written to the node from outside is honored", "[character]") {
    CharacterWorld w;
    w.add_floor();
    auto id = w.add_character({0.0f, 2.0f, 0.0f});
    w.start();
    REQUIRE(w.settle(id));

    w.node(id).transform.position.x = 3.0f;
    w.step(1);
    REQUIRE(w.node(id).transform.position.x == Approx(3.0f).margin(0.05f));
}
