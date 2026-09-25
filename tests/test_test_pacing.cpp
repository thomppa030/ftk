#include <catch2/catch_test_macros.hpp>

#include "scene/scene.hpp"
#include "scripting/test_pacing.hpp"

#include <string>
#include <vector>

using namespace fjell;

TEST_CASE("Pacings are frame rates and jitter, at least two", "[test_pacing]") {
    const auto pacings = parse_pacings("30,144,jitter");
    REQUIRE(pacings);
    REQUIRE(pacings->size() == 3);
    CHECK((*pacings)[0].name == "30 fps");
    CHECK((*pacings)[0].fps == 30.0);
    CHECK((*pacings)[1].name == "144 fps");
    CHECK((*pacings)[2].name == "jitter");
    CHECK((*pacings)[2].fps == 0.0);

    CHECK(parse_pacings("59.94,60"));
    CHECK_FALSE(parse_pacings("60"));          // nothing to compare with
    CHECK_FALSE(parse_pacings("60,sixty"));
    CHECK_FALSE(parse_pacings("0,60"));
    CHECK_FALSE(parse_pacings("60,60"));
    CHECK_FALSE(parse_pacings("30,,60"));
}

TEST_CASE("Paced frames are the pacing's length, or the same uneven ones every run",
          "[test_pacing]") {
    PacedFrames thirty(Pacing{.name = "30 fps", .fps = 30.0});
    CHECK(thirty.next() == 1.0 / 30.0);
    CHECK(thirty.next() == 1.0 / 30.0);

    PacedFrames first(Pacing{.name = "jitter", .fps = 0.0});
    PacedFrames second(Pacing{.name = "jitter", .fps = 0.0});
    bool uneven = false;
    double previous = -1.0;
    for (int i = 0; i < 200; ++i) {
        const double frame = first.next();
        CHECK(frame == second.next());
        CHECK(frame > 0.0);
        if (previous >= 0.0 && frame != previous) uneven = true;
        previous = frame;
    }
    CHECK(uneven);
}

namespace {

// A recording of a scene with one node moved on every step, `nudge` added to
// its position from step `from` on.
[[nodiscard]] StepRecording crate_run(float nudge, std::size_t from) {
    Scene scene;
    const uint32_t crate = scene.add_empty({}, "Crate");
    (void)scene.add_empty({}, "Wall");  // never moves, so never recorded
    StepRecording recording;
    for (std::uint64_t step = 0; step < 5; ++step) {
        scene.begin_fixed_step(step);
        auto& position = scene.find_spatial(crate)->transform.position;
        position.x += 1.0f;
        if (step >= from) position.x += nudge;
        recording.record(scene, nullptr);
    }
    return recording;
}

} // namespace

TEST_CASE("Paced runs that step alike do not diverge", "[test_pacing]") {
    const StepRecording a = crate_run(0.0f, 0);
    const StepRecording b = crate_run(0.0f, 0);
    CHECK(a.steps() == 5);
    CHECK_FALSE(a.first_divergence(b).has_value());
}

TEST_CASE("A divergence names the step, the node and the values", "[test_pacing]") {
    const StepRecording a = crate_run(0.0f, 0);
    const StepRecording b = crate_run(0.25f, 3);
    const auto divergence = a.first_divergence(b);
    REQUIRE(divergence);
    CHECK(divergence->step == 3);
    CHECK(divergence->node == "Crate");
    CHECK(divergence->field == "position x");
    CHECK(divergence->first == "4");
    CHECK(divergence->second == "4.25");
}

TEST_CASE("A node that moved in one run and not the other is a divergence", "[test_pacing]") {
    Scene scene;
    const uint32_t door = scene.add_empty({}, "Door");
    StepRecording still;
    StepRecording opened;
    scene.begin_fixed_step(0);
    still.record(scene, nullptr);
    scene.find_spatial(door)->transform.position.y = 2.0f;
    opened.record(scene, nullptr);

    const auto divergence = still.first_divergence(opened);
    REQUIRE(divergence);
    CHECK(divergence->step == 0);
    CHECK(divergence->node == "Door");
    CHECK(divergence->first == "no");
    CHECK(divergence->second == "yes");
}
