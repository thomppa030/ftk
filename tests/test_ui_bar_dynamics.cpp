#include "ui/game_ui/ui_bar_dynamics.hpp"
#include "ui/game_ui/ui_node.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>

using namespace fjell;

namespace {

std::unique_ptr<UiNode> make_bar(float fill) {
    auto node = std::make_unique<UiNode>();
    node->tag = "bar";
    node->fill_fraction = fill;
    return node;
}

// Runs the dynamics for `seconds` at 120 Hz, returning whether the last
// step still reported motion.
bool run(UiBarDynamics& dynamics, UiNode& root, float seconds) {
    bool active = false;
    const float dt = 1.0f / 120.0f;
    for (float t = 0.0f; t < seconds; t += dt) {
        active = dynamics.update(root, dt);
    }
    return active;
}

} // namespace

TEST_CASE("A bar first seen starts at its value, at rest", "[ui][bar]") {
    UiBarDynamics dynamics;
    auto bar = make_bar(0.4f);
    REQUIRE_FALSE(dynamics.update(*bar, 1.0f / 60.0f));
    REQUIRE(bar->fill_display == Catch::Approx(0.4f));
    REQUIRE(bar->fill_velocity == Catch::Approx(0.0f));
    REQUIRE(bar->fill_agitation == Catch::Approx(0.0f));
}

TEST_CASE("The displayed fill follows the value and comes to rest", "[ui][bar]") {
    UiBarDynamics dynamics;
    auto bar = make_bar(1.0f);
    dynamics.update(*bar, 1.0f / 60.0f);

    bar->fill_fraction = 0.5f;
    REQUIRE(dynamics.update(*bar, 1.0f / 60.0f));
    // One frame in, the display has barely left its old value.
    REQUIRE(bar->fill_display > 0.9f);
    REQUIRE(bar->fill_velocity < 0.0f);

    bool active = run(dynamics, *bar, 2.0f);
    REQUIRE_FALSE(active);
    REQUIRE(bar->fill_display == Catch::Approx(0.5f));
    REQUIRE(bar->fill_velocity == Catch::Approx(0.0f));
}

TEST_CASE("A drop stirs the bar harder than a rise, and both settle", "[ui][bar]") {
    UiBarDynamics dynamics;
    auto bar = make_bar(1.0f);
    dynamics.update(*bar, 1.0f / 60.0f);

    bar->fill_fraction = 0.6f;
    dynamics.update(*bar, 1.0f / 60.0f);
    float after_drop = bar->fill_agitation;
    REQUIRE(after_drop > 0.5f);
    run(dynamics, *bar, 3.0f);
    REQUIRE(bar->fill_agitation == Catch::Approx(0.0f));

    bar->fill_fraction = 1.0f;
    dynamics.update(*bar, 1.0f / 60.0f);
    REQUIRE(bar->fill_agitation > 0.0f);
    REQUIRE(bar->fill_agitation < after_drop);
}

TEST_CASE("bounce overshoots and settle bounds the time to rest", "[ui][bar]") {
    UiBarDynamics stiff_dynamics;
    auto stiff = make_bar(1.0f);
    stiff->attributes["bounce"] = "0";
    stiff_dynamics.update(*stiff, 1.0f / 60.0f);
    stiff->fill_fraction = 0.2f;
    float lowest = 1.0f;
    const float dt = 1.0f / 120.0f;
    for (float t = 0.0f; t < 2.0f; t += dt) {
        stiff_dynamics.update(*stiff, dt);
        lowest = std::min(lowest, stiff->fill_display);
    }
    REQUIRE(lowest >= 0.2f - 0.002f);   // critically damped: no overshoot

    UiBarDynamics loose_dynamics;
    auto loose = make_bar(1.0f);
    loose->attributes["bounce"] = "1";
    loose_dynamics.update(*loose, 1.0f / 60.0f);
    loose->fill_fraction = 0.2f;
    lowest = 1.0f;
    for (float t = 0.0f; t < 2.0f; t += dt) {
        loose_dynamics.update(*loose, dt);
        lowest = std::min(lowest, loose->fill_display);
    }
    REQUIRE(lowest < 0.2f - 0.02f);     // visibly past the target on the way

    // A short settle brings the position in fast; the disturbance a big
    // drop leaves behind dies away on its own clock.
    UiBarDynamics quick_dynamics;
    auto quick = make_bar(1.0f);
    quick->attributes["settle"] = "0.1";
    quick_dynamics.update(*quick, 1.0f / 60.0f);
    quick->fill_fraction = 0.3f;
    run(quick_dynamics, *quick, 0.3f);
    REQUIRE(quick->fill_display == Catch::Approx(0.3f).margin(0.003f));
    REQUIRE_FALSE(run(quick_dynamics, *quick, 2.0f));
}

TEST_CASE("Bars nested in the tree are all advanced", "[ui][bar]") {
    UiBarDynamics dynamics;
    auto root = std::make_unique<UiNode>();
    root->tag = "div";
    auto bar = make_bar(1.0f);
    UiNode* raw = bar.get();
    root->children.push_back(std::move(bar));
    dynamics.update(*root, 1.0f / 60.0f);
    raw->fill_fraction = 0.0f;
    REQUIRE(dynamics.update(*root, 1.0f / 60.0f));
    REQUIRE(raw->fill_display < 1.0f);
}

TEST_CASE("forget_tree drops state keyed by node address", "[ui][bar]") {
    UiBarDynamics dynamics;
    auto bar = make_bar(1.0f);
    dynamics.update(*bar, 1.0f / 60.0f);
    bar->fill_fraction = 0.2f;
    dynamics.update(*bar, 1.0f / 60.0f);
    REQUIRE(bar->fill_agitation > 0.0f);

    dynamics.forget_tree();
    // Seen fresh, the bar sits at its value with nothing stirred up.
    REQUIRE_FALSE(dynamics.update(*bar, 1.0f / 60.0f));
    REQUIRE(bar->fill_display == Catch::Approx(0.2f));
    REQUIRE(bar->fill_agitation == Catch::Approx(0.0f));
}
