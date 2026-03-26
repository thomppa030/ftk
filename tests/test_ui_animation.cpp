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

    root->style.opacity = 0.0f;
    anim.update(*root, 0.0f); // detect change

    root->style.opacity = 0.0f;
    anim.update(*root, 0.5f); // 50% through
    REQUIRE(root->style.opacity == Catch::Approx(0.5f).margin(0.05f));

    root->style.opacity = 0.0f;
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

    // At 25% time, ease-in (t²) = 0.0625 → still near start
    root->style.opacity = 0.0f;
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

    // At 25% time, ease-out t(2-t) = 0.4375 → already moved a lot
    root->style.opacity = 0.0f;
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

    root->style.opacity = 0.0f;
    anim.update(*root, 0.15f); // 50% of 0.3s
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

    root->style.opacity = 0.0f;
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

    root->style.background_color = {0.0f, 0.0f, 1.0f, 1.0f};
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

    root->style.opacity = 0.0f;
    root->style.border_width = 4.0f;
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

    child->style.opacity = 0.0f; // only child changes
    anim.update(*root, 0.0f);

    child->style.opacity = 0.0f;
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
