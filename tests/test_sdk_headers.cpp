// The script SDK headers are shipped to game projects and never compiled by
// the engine itself, which includes the engine's own component types instead.
// This translation unit sees them the way a game module does, so a broken
// header fails here rather than in a project's build.
#include <catch2/catch_test_macros.hpp>

#include "fjell/registry.hpp"
#include "fjell/ref.hpp"
#include "fjell/environment_api.hpp"
#include "fjell/scene_api.hpp"
#include "fjell/engine_components.hpp"
#include "fjell/physics_api.hpp"

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

namespace {

// Stand-ins for what a game project declares. FJELL_COMPONENT is deliberately
// absent: it registers the type with a running engine, and these only need to
// be what register_component<T>() reads.
struct SampleTest : fjell::FunctionalTest {
    static constexpr const char* type_name = "SampleTest";
};

struct SamplePlain : fjell::ScriptComponent {
    static constexpr const char* type_name = "SamplePlain";
};

/// What register_component<T>() writes into the descriptor's flag.
template <class T>
constexpr bool registered_as_test() {
    return std::is_base_of_v<fjell::FunctionalTest, T>;
}

} // namespace

TEST_CASE("A functional test is distinguishable from a plain component", "[sdk]") {
    // The C ABI hands the engine a ScriptComponent* with no base-class
    // information, so this flag is the only thing making the runner's cast
    // sound. Getting it wrong reads a FunctionalTest's members off a component
    // that has none.
    STATIC_REQUIRE(registered_as_test<SampleTest>());
    STATIC_REQUIRE_FALSE(registered_as_test<SamplePlain>());

    // The descriptor has to carry the flag across to the engine at all.
    FjellComponentTypeOOP desc{};
    REQUIRE_FALSE(desc.is_functional_test);
    desc.is_functional_test = registered_as_test<SampleTest>();
    REQUIRE(desc.is_functional_test);
}

namespace {

// What register_component<T>() reads to decide which polled hooks a type is
// walked for.
struct Idle : fjell::ScriptComponent {};
struct Ticker : fjell::ScriptComponent {
    void tick(float) override {}
};
struct Follower : Ticker {};
struct PrivateStepper : fjell::ScriptComponent {
private:
    void fixed_tick(const fjell::FixedStep&) override {}
};
struct Watcher : fjell::ScriptComponent {
    void late_tick(float) override {}
};

} // namespace

TEST_CASE("A script type is walked only for the polled hooks it overrides", "[sdk]") {
    STATIC_REQUIRE(fjell::inherits_tick<Idle>);
    STATIC_REQUIRE(fjell::inherits_fixed_tick<Idle>);
    STATIC_REQUIRE(fjell::inherits_late_tick<Idle>);

    STATIC_REQUIRE_FALSE(fjell::inherits_tick<Ticker>);
    STATIC_REQUIRE(fjell::inherits_fixed_tick<Ticker>);
    STATIC_REQUIRE(fjell::inherits_late_tick<Ticker>);
    STATIC_REQUIRE_FALSE(fjell::inherits_late_tick<Watcher>);
    // An override from a base of the type's own is still an override.
    STATIC_REQUIRE_FALSE(fjell::inherits_tick<Follower>);
    // So is one the type keeps private: it cannot be named, and is called.
    STATIC_REQUIRE_FALSE(fjell::inherits_fixed_tick<PrivateStepper>);
    // A functional test runs its test hooks from the polled ones.
    STATIC_REQUIRE_FALSE(fjell::inherits_tick<SampleTest>);
    STATIC_REQUIRE_FALSE(fjell::inherits_fixed_tick<SampleTest>);
}

TEST_CASE("A Ref is its data and nothing more", "[sdk]") {
    // The engine reads and writes a Ref field in place as a RefData, for
    // every T alike.
    STATIC_REQUIRE(sizeof(fjell::Ref<SamplePlain>) == sizeof(fjell::RefData));
    STATIC_REQUIRE(sizeof(fjell::Ref<Idle>) == sizeof(fjell::RefData));
    STATIC_REQUIRE(std::is_standard_layout_v<fjell::Ref<SamplePlain>>);
}

namespace sdk_check {

// A script type with external linkage: the lookups instantiated for it are
// emitted only where they are called, and the check below calls nothing.
struct Carried : fjell::ScriptComponent {
    static constexpr const char* type_name = "Carried";
};

// The collision callbacks as a script overrides them.
struct Toucher : fjell::ScriptComponent {
    void on_collision_enter(fjell::Ref<fjell::Node3D> other, const glm::vec3&, const glm::vec3&) override {
        (void)other;
    }
    void on_trigger_exit(fjell::Ref<fjell::Node3D> other) override { (void)other; }
};

} // namespace sdk_check

TEST_CASE("The node API reads as a script writes it", "[sdk]") {
    // Type-checked here and never run: it would call into the engine.
    [[maybe_unused]] auto script = [](fjell::ScriptComponent& self_script, const fjell::Asset<fjell::Preset>& preset) {
        fjell::Node3D& own = self_script.self();
        own.position().y += 1.0f;
        fjell::Ref<fjell::Node3D> door = fjell::scene::find_node("Door");
        door->rotation() = own.rotation();
        if (auto body = door->get_component<fjell::RigidbodyComponent>()) body->mass = 5.0f;
        const fjell::Ref<sdk_check::Carried> sample = fjell::scene::find<sdk_check::Carried>();
        const fjell::Ref<fjell::Node3D> holder = sample.node();
        fjell::Ref<fjell::Node3D> spawned = fjell::scene::spawn(preset, holder->position(), own);
        spawned->find_child("Weapon")->destroy();
        [[maybe_unused]] fjell::Ref<sdk_check::Carried> added = own.add_component<sdk_check::Carried>();
        [[maybe_unused]] fjell::Ref<fjell::Node3D> mine = own;  // a script's own node where a Ref is taken

        // A body through its component; the world through physics::.
        auto body = own.get_component<fjell::RigidbodyComponent>();
        body->add_force({0.0f, 1.0f, 0.0f});
        body->add_impulse({0.0f, 1.0f, 0.0f});
        body->set_linear_velocity(body->linear_velocity());
        body->set_angular_velocity(body->angular_velocity());
        fjell::PhysicsHit hit;
        if (fjell::physics::raycast(own.position(), {0.0f, -1.0f, 0.0f}, 1.0f, hit, {FJ_MASK_ALL, own})) {
            hit.node->destroy();
        }
        for (const fjell::Ref<fjell::Node3D>& near : fjell::physics::overlap_sphere(own.position(), 2.0f)) {
            (void)near;
        }
    };
    // A Node3D is the node itself: never a copy.
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<fjell::Node3D>);
    STATIC_REQUIRE_FALSE(std::is_default_constructible_v<fjell::Node3D>);
    // A Ref<Node3D> is a Ref like any other.
    STATIC_REQUIRE(sizeof(fjell::Ref<fjell::Node3D>) == sizeof(fjell::RefData));
}

TEST_CASE("An Asset is its data and nothing more", "[sdk]") {
    // The engine reads and writes an Asset field in place as an AssetData,
    // and reads one handed to an SDK call with nothing there the same way.
    STATIC_REQUIRE(sizeof(fjell::Asset<fjell::Weather>) == sizeof(fjell::AssetData));
    STATIC_REQUIRE(std::is_standard_layout_v<fjell::Asset<fjell::Weather>>);
}

TEST_CASE("A test reports its own verdict", "[sdk]") {
    struct Probe : fjell::FunctionalTest {
        using fjell::FunctionalTest::assert_equal;
        using fjell::FunctionalTest::assert_true;
        using fjell::FunctionalTest::finish_test;
    };

    SECTION("a fresh test is running and has nothing to report") {
        Probe t;
        REQUIRE_FALSE(t.finished());
        REQUIRE(t.test_result() == fjell::TestResult::running);
        REQUIRE(t.failures().empty());
    }

    SECTION("a failed assertion records but does not end the test") {
        Probe t;
        REQUIRE_FALSE(t.assert_true(false, "the door opened"));
        REQUIRE_FALSE(t.finished());
        REQUIRE(t.failures().size() == 1);
        REQUIRE(t.failures()[0] == "the door opened: expected true, got false");
    }

    SECTION("every failure is reported, not just the first") {
        Probe t;
        t.assert_equal(1.0f, 2.0f, "height");
        t.assert_equal(glm::vec3{0.0f}, glm::vec3{1.0f, 0.0f, 0.0f}, "position");
        REQUIRE(t.failures().size() == 2);
        REQUIRE(t.failures()[0] == "height: expected 2.0000, got 1.0000");
        REQUIRE(t.failures()[1] ==
                "position: expected (1.0000, 0.0000, 0.0000), got (0.0000, 0.0000, 0.0000)");
    }

    SECTION("a tolerance is what makes a float comparison usable") {
        Probe t;
        REQUIRE(t.assert_equal(1.0f, 1.00005f, "close enough", 1e-3f));
        REQUIRE(t.failures().empty());
    }

    SECTION("a test that failed an assertion cannot then pass") {
        Probe t;
        t.assert_true(false, "the trap fired");
        t.finish_test(fjell::TestResult::passed, "done");
        REQUIRE(t.test_result() == fjell::TestResult::failed);
    }

    SECTION("a test is only ticked between start_test and its verdict") {
        // The script subsystem ticks every component every frame, including
        // while a test is still waiting on is_ready(). A test that drove the
        // world in that window would be moving things before it started
        // measuring — which is exactly what made the trap test report a walk
        // shorter than the one it performed.
        struct Counter : fjell::FunctionalTest {
            using fjell::FunctionalTest::finish_test;
            int ticks = 0;
            void test_tick(float) override { ++ticks; }
        };

        Counter t;
        t.tick(0.016f);
        REQUIRE(t.ticks == 0);   // before begin_test(): not running yet

        t.begin_test();
        t.tick(0.016f);
        t.tick(0.016f);
        REQUIRE(t.ticks == 2);

        t.finish_test(fjell::TestResult::passed, "done");
        t.tick(0.016f);
        REQUIRE(t.ticks == 2);   // after the verdict: not ticked again
    }

    SECTION("the first verdict wins") {
        Probe t;
        t.finish_test(fjell::TestResult::passed, "first");
        t.finish_test(fjell::TestResult::failed, "second");
        REQUIRE(t.test_result() == fjell::TestResult::passed);
        REQUIRE(t.test_message() == "first");
    }
}
