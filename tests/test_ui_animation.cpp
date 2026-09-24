#include "ui/game_ui/ui_animation.hpp"
#include "ui/game_ui/ui_node.hpp"
#include "core/log.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

using namespace fjell;

static struct LogInit {
    LogInit() { if (!log::core()) log::init(); }
} s_log_init;

static std::unique_ptr<UiNode> make_node(const std::string& tag) {
    auto n = std::make_unique<UiNode>();
    n->type = UiNodeType::element;
    n->tag = tag;
    return n;
}

static UiNode* add_child(UiNode& parent, std::unique_ptr<UiNode> child) {
    child->parent = &parent;
    parent.children.push_back(std::move(child));
    return parent.children.back().get();
}

// --- Easing math ---

TEST_CASE("Linear easing: halfway through gives halfway value", "[anim]") {
    auto root = make_node("div");
    root->style.transition = "opacity 1.0s linear";
    root->style.opacity = 1.0f;

    UiAnimation anim;
    anim.update(*root, 0.0f); // snapshot

    root->style.opacity = 0.0f; // style resolver sets target
    anim.update(*root, 0.0f); // detect change, start transition

    // Don't re-set opacity — animation wrote the interpolated value back,
    // and prev_style was snapshotted. Let the transition play out.
    anim.update(*root, 0.5f); // 50% through
    REQUIRE(root->style.opacity == Catch::Approx(0.5f).margin(0.05f));

    anim.update(*root, 0.5f); // complete
    REQUIRE(root->style.opacity == Catch::Approx(0.0f));
}

TEST_CASE("Ease-in starts slow", "[anim]") {
    auto root = make_node("div");
    root->style.transition = "opacity 1.0s ease-in";
    root->style.opacity = 1.0f;

    UiAnimation anim;
    anim.update(*root, 0.0f);

    root->style.opacity = 0.0f;
    anim.update(*root, 0.0f);

    anim.update(*root, 0.25f);
    REQUIRE(root->style.opacity > 0.9f);
}

TEST_CASE("Ease-out starts fast", "[anim]") {
    auto root = make_node("div");
    root->style.transition = "opacity 1.0s ease-out";
    root->style.opacity = 1.0f;

    UiAnimation anim;
    anim.update(*root, 0.0f);

    root->style.opacity = 0.0f;
    anim.update(*root, 0.0f);

    anim.update(*root, 0.25f);
    REQUIRE(root->style.opacity < 0.6f);
}

// --- Transition lifecycle ---

TEST_CASE("No transition on first frame", "[anim]") {
    auto root = make_node("div");
    root->style.transition = "all 0.3s ease-in-out";
    root->style.opacity = 0.5f;

    UiAnimation anim;
    bool active = anim.update(*root, 0.016f);
    REQUIRE(active == false);
    REQUIRE(root->style.opacity == Catch::Approx(0.5f));
}

TEST_CASE("Transition starts when property changes", "[anim]") {
    auto root = make_node("div");
    root->style.transition = "opacity 0.5s linear";
    root->style.opacity = 1.0f;

    UiAnimation anim;
    anim.update(*root, 0.0f);

    root->style.opacity = 0.0f;
    bool active = anim.update(*root, 0.016f);
    REQUIRE(active == true);
    REQUIRE(root->style.opacity > 0.0f); // interpolating, not snapped
}

TEST_CASE("Transition completes and snaps to target", "[anim]") {
    auto root = make_node("div");
    root->style.transition = "opacity 0.1s linear";
    root->style.opacity = 1.0f;

    UiAnimation anim;
    anim.update(*root, 0.0f);

    root->style.opacity = 0.0f;
    anim.update(*root, 0.0f);

    root->style.opacity = 0.0f;
    bool active = anim.update(*root, 0.2f);
    REQUIRE(root->style.opacity == Catch::Approx(0.0f));
    REQUIRE(active == false);
}

TEST_CASE("Returns false when no transitions are active", "[anim]") {
    auto root = make_node("div");
    root->style.transition = "all 0.3s linear";
    root->style.opacity = 1.0f;

    UiAnimation anim;
    REQUIRE(anim.update(*root, 0.0f) == false);
    REQUIRE(anim.update(*root, 0.0f) == false);
}

// --- Duration parsing ---

TEST_CASE("Duration in seconds", "[anim]") {
    auto root = make_node("div");
    root->style.transition = "opacity 0.3s linear";
    root->style.opacity = 1.0f;

    UiAnimation anim;
    anim.update(*root, 0.0f);

    root->style.opacity = 0.0f;
    anim.update(*root, 0.0f);

    anim.update(*root, 0.15f);
    REQUIRE(root->style.opacity == Catch::Approx(0.5f).margin(0.05f));
}

TEST_CASE("Duration in milliseconds", "[anim]") {
    auto root = make_node("div");
    root->style.transition = "opacity 300ms linear";
    root->style.opacity = 1.0f;

    UiAnimation anim;
    anim.update(*root, 0.0f);

    root->style.opacity = 0.0f;
    anim.update(*root, 0.0f);

    anim.update(*root, 0.15f);
    REQUIRE(root->style.opacity == Catch::Approx(0.5f).margin(0.05f));
}

// --- Color transitions ---

TEST_CASE("Background color transition interpolates RGBA", "[anim]") {
    auto root = make_node("div");
    root->style.transition = "all 1.0s linear";
    root->style.background_color = {1.0f, 0.0f, 0.0f, 1.0f};

    UiAnimation anim;
    anim.update(*root, 0.0f);

    root->style.background_color = {0.0f, 0.0f, 1.0f, 1.0f};
    anim.update(*root, 0.0f);

    anim.update(*root, 0.5f);

    REQUIRE(root->style.background_color.r == Catch::Approx(0.5f).margin(0.05f));
    REQUIRE(root->style.background_color.b == Catch::Approx(0.5f).margin(0.05f));
}

// --- "all" keyword ---

TEST_CASE("'all' transitions multiple properties simultaneously", "[anim]") {
    auto root = make_node("div");
    root->style.transition = "all 1.0s linear";
    root->style.opacity = 1.0f;
    root->style.border_width = 0.0f;

    UiAnimation anim;
    anim.update(*root, 0.0f);

    root->style.opacity = 0.0f;
    root->style.border_width = 4.0f;
    anim.update(*root, 0.0f);

    anim.update(*root, 0.5f);

    REQUIRE(root->style.opacity == Catch::Approx(0.5f).margin(0.05f));
    REQUIRE(root->style.border_width == Catch::Approx(2.0f).margin(0.1f));
}

// --- Children ---

TEST_CASE("Transitions apply to children independently", "[anim]") {
    auto root = make_node("div");
    auto* child = add_child(*root, make_node("span"));

    root->style.transition = "opacity 1.0s linear";
    root->style.opacity = 1.0f;
    child->style.transition = "opacity 1.0s linear";
    child->style.opacity = 1.0f;

    UiAnimation anim;
    anim.update(*root, 0.0f);

    child->style.opacity = 0.0f;
    anim.update(*root, 0.0f);

    anim.update(*root, 0.5f);

    REQUIRE(root->style.opacity == Catch::Approx(1.0f));
    REQUIRE(child->style.opacity == Catch::Approx(0.5f).margin(0.05f));
}

// --- Edge cases ---

TEST_CASE("Zero duration snaps immediately", "[anim]") {
    auto root = make_node("div");
    root->style.transition = "opacity 0s linear";
    root->style.opacity = 1.0f;

    UiAnimation anim;
    anim.update(*root, 0.0f);

    root->style.opacity = 0.0f;
    anim.update(*root, 0.016f);
    REQUIRE(root->style.opacity == Catch::Approx(0.0f));
}

TEST_CASE("Small change below threshold doesn't trigger transition", "[anim]") {
    auto root = make_node("div");
    root->style.transition = "opacity 1.0s linear";
    root->style.opacity = 1.0f;

    UiAnimation anim;
    anim.update(*root, 0.0f);

    root->style.opacity = 1.0005f; // delta < 0.001
    bool active = anim.update(*root, 0.016f);
    REQUIRE(active == false);
}

TEST_CASE("Node without transition spec: no interpolation", "[anim]") {
    auto root = make_node("div");
    root->style.opacity = 1.0f;

    UiAnimation anim;
    anim.update(*root, 0.0f);
    root->style.opacity = 0.0f;
    anim.update(*root, 0.016f);
    REQUIRE(root->style.opacity == Catch::Approx(0.0f));
}

// --- Any animatable property ---

TEST_CASE("transition: width interpolates the layout width", "[anim]") {
    auto root = make_node("div");
    root->style.transition = "width 1.0s linear";
    root->style.width = UiLength::pixels(100.0f);

    UiAnimation anim;
    anim.update(*root, 0.0f);

    root->style.width = UiLength::pixels(200.0f);
    anim.update(*root, 0.0f);
    REQUIRE(anim.update(*root, 0.5f));
    REQUIRE(root->style.width.value == Catch::Approx(150.0f));
}

TEST_CASE("transition: margin covers every side", "[anim]") {
    auto root = make_node("div");
    root->style.transition = "margin 1.0s linear";
    root->style.margin = UiEdges::all(UiLength::pixels(0.0f));

    UiAnimation anim;
    anim.update(*root, 0.0f);

    root->style.margin = {UiLength::pixels(10.0f), UiLength::pixels(20.0f),
                          UiLength::pixels(30.0f), UiLength::pixels(40.0f)};
    anim.update(*root, 0.0f);
    anim.update(*root, 0.5f);
    REQUIRE(root->style.margin.top.value == Catch::Approx(5.0f));
    REQUIRE(root->style.margin.left.value == Catch::Approx(20.0f));
}

TEST_CASE("transition: border-radius interpolates all four corners", "[anim]") {
    auto root = make_node("div");
    root->style.transition = "border-radius 1.0s linear";
    root->style.border_radius = {0.0f, 0.0f, 0.0f, 0.0f};

    UiAnimation anim;
    anim.update(*root, 0.0f);

    root->style.border_radius = {8.0f, 8.0f, 8.0f, 8.0f};
    anim.update(*root, 0.0f);
    anim.update(*root, 0.25f);
    REQUIRE(root->style.border_radius.bottom_left == Catch::Approx(2.0f));
}

TEST_CASE("A resolve to the same target mid-flight does not restart the transition", "[anim]") {
    auto root = make_node("div");
    root->style.transition = "opacity 1.0s linear";
    root->style.opacity = 0.0f;
    UiAnimation anim;
    anim.update(*root, 0.0f);

    root->style.opacity = 1.0f;
    anim.update(*root, 0.0f);
    anim.update(*root, 0.5f);
    REQUIRE(root->style.opacity == Catch::Approx(0.5f));

    // The cascade re-resolves the node (hover on a sibling, a data-bind
    // relayout) and writes the target again.
    root->style.opacity = 1.0f;
    anim.update(*root, 0.25f);
    REQUIRE(root->style.opacity == Catch::Approx(0.75f));
}

TEST_CASE("Retargeting mid-flight continues from the displayed value", "[anim]") {
    auto root = make_node("div");
    root->style.transition = "opacity 1.0s linear";
    root->style.opacity = 0.0f;
    UiAnimation anim;
    anim.update(*root, 0.0f);

    root->style.opacity = 1.0f;
    anim.update(*root, 0.0f);
    anim.update(*root, 0.5f);
    REQUIRE(root->style.opacity == Catch::Approx(0.5f));

    // Hover ends halfway: head back to 0 from 0.5, not from 1.
    root->style.opacity = 0.0f;
    anim.update(*root, 0.0f);
    anim.update(*root, 0.5f);
    REQUIRE(root->style.opacity == Catch::Approx(0.25f));
}

TEST_CASE("A length that changes unit snaps instead of interpolating", "[anim]") {
    auto root = make_node("div");
    root->style.transition = "width 1.0s linear";
    root->style.width = UiLength::pixels(100.0f);
    UiAnimation anim;
    anim.update(*root, 0.0f);

    root->style.width = UiLength::percent(50.0f);
    anim.update(*root, 0.0f);
    REQUIRE_FALSE(anim.update(*root, 0.5f));
    REQUIRE(root->style.width == UiLength::percent(50.0f));
}

TEST_CASE("Fading a container fades what it holds, frame by frame", "[anim][opacity]") {
    auto panel = make_node("div");
    panel->style.transition = "opacity 0.4s linear";
    auto* label = add_child(*panel, make_node("span"));
    auto* bar = add_child(*label, make_node("bar"));
    bar->style.opacity = 0.8f; // its own, which the fade multiplies

    UiAnimation anim;
    anim.update(*panel, 0.0f); // snapshot at full opacity
    panel->style.opacity = 0.0f; // the cascade sets the target, e.g. under .calm
    anim.update(*panel, 0.0f); // the transition starts

    anim.update(*panel, 0.2f); // halfway
    ui_resolve_draw_opacity(*panel);
    REQUIRE(label->draw_opacity == Catch::Approx(0.5f).margin(0.05f));
    REQUIRE(bar->draw_opacity == Catch::Approx(0.4f).margin(0.05f));

    anim.update(*panel, 0.2f); // done
    ui_resolve_draw_opacity(*panel);
    REQUIRE(label->draw_opacity == Catch::Approx(0.0f));
    REQUIRE(bar->draw_opacity == Catch::Approx(0.0f));
}
