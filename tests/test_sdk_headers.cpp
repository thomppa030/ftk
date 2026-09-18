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

    SECTION("the first verdict wins") {
        Probe t;
        t.finish_test(fjell::TestResult::passed, "first");
        t.finish_test(fjell::TestResult::failed, "second");
        REQUIRE(t.test_result() == fjell::TestResult::passed);
        REQUIRE(t.test_message() == "first");
    }
}
