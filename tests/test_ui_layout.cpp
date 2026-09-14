#include "ui/game_ui/ui_layout.hpp"
#include "ui/game_ui/fjss_parser.hpp"
#include "ui/game_ui/style_resolver.hpp"
#include "ui/game_ui/ui_node.hpp"
#include "ui_length_printer.hpp"
#include "core/log.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

using namespace fjell;

static struct LogInit {
    LogInit() { if (!log::core()) log::init(); }
} s_log_init;

static std::unique_ptr<UiNode> make_node(const std::string& tag, std::vector<std::string> classes = {}) {
    auto n = std::make_unique<UiNode>();
    n->type = UiNodeType::element;
    n->tag = tag;
    n->classes = std::move(classes);
    return n;
}

static UiNode* add_child(UiNode& parent, std::unique_ptr<UiNode> child) {
    child->parent = &parent;
    parent.children.push_back(std::move(child));
    return parent.children.back().get();
}

// A 400x300 root with one styled child, laid out from the given rules.
struct LaidOut {
    std::unique_ptr<UiNode> root;
    UiNode* child{nullptr};
    UiLayout layout;

    explicit LaidOut(const char* css, const char* root_css = "") {
        root = make_node("panel", {"root"});
        child = add_child(*root, make_node("div", {"a"}));
        std::string sheet_text = std::string(".root { ") + root_css + " } .a { " + css + " }";
        auto sheet = FjssParser::parse(sheet_text);
        REQUIRE(sheet.diagnostics.empty());
        StyleResolver resolver;
        resolver.resolve(*root, sheet);
        layout.compute(*root, 400.0f, 300.0f);
    }
};

TEST_CASE("Percent width resolves against the parent", "[layout]") {
    LaidOut l("width: 50%; height: 10;");
    REQUIRE(l.child->layout_rect.w == Catch::Approx(200.0f));
}

TEST_CASE("Pixel width is taken as is", "[layout]") {
    LaidOut l("width: 120px; height: 10;");
    REQUIRE(l.child->layout_rect.w == Catch::Approx(120.0f));
}

TEST_CASE("Percent padding resolves and is reported in pixels on the node", "[layout]") {
    LaidOut l("width: 100%; height: 100; padding: 10%;");
    // CSS and Yoga resolve every side of a percent padding against the width
    REQUIRE(l.child->layout_padding.left == Catch::Approx(40.0f));
    REQUIRE(l.child->layout_padding.top == Catch::Approx(40.0f));
}

TEST_CASE("Margin auto centers a fixed-width child in a row", "[layout]") {
    LaidOut l("width: 100; height: 10; margin-left: auto; margin-right: auto;", "flex-direction: row;");
    REQUIRE(l.child->layout_rect.x == Catch::Approx(150.0f));
    // Yoga reports an auto margin as 0 through its layout getter; the
    // position above is the observable result.
}

TEST_CASE("top: 0 pins an absolute child to the top edge", "[layout]") {
    LaidOut l("position: absolute; top: 0; left: 0; width: 10; height: 10;", "padding: 20;");
    REQUIRE(l.child->layout_rect.y == Catch::Approx(0.0f));
    REQUIRE(l.child->layout_rect.x == Catch::Approx(0.0f));
}

TEST_CASE("An unset inset leaves the absolute child at its flow position", "[layout]") {
    LaidOut l("position: absolute; width: 10; height: 10;", "padding: 20;");
    REQUIRE(l.child->layout_rect.y == Catch::Approx(20.0f));
}

TEST_CASE("max-width with a percentage caps the child", "[layout]") {
    LaidOut l("width: 100%; max-width: 25%; height: 10;");
    REQUIRE(l.child->layout_rect.w == Catch::Approx(100.0f));
}

TEST_CASE("vw and vh resolve against the viewport", "[layout][units]") {
    LaidOut l("width: 10vw; height: 50vh;");
    REQUIRE(l.child->layout_rect.w == Catch::Approx(40.0f));
    REQUIRE(l.child->layout_rect.h == Catch::Approx(150.0f));
}

TEST_CASE("em resolves against the node's own font size", "[layout][units]") {
    LaidOut l("font-size: 20; width: 2em; height: 1em;");
    REQUIRE(l.child->layout_rect.w == Catch::Approx(40.0f));
    REQUIRE(l.child->layout_rect.h == Catch::Approx(20.0f));
}

TEST_CASE("rem resolves against the root's font size", "[layout][units]") {
    LaidOut l("font-size: 40; width: 2rem; height: 10;", "font-size: 12;");
    REQUIRE(l.child->layout_rect.w == Catch::Approx(24.0f));
}

TEST_CASE("vmin picks the smaller viewport side", "[layout][units]") {
    LaidOut l("width: 10vmin; height: 10vmax;");
    REQUIRE(l.child->layout_rect.w == Catch::Approx(30.0f));
    REQUIRE(l.child->layout_rect.h == Catch::Approx(40.0f));
}

// --- Design frame ---

TEST_CASE("Design frame scales by height on a viewport at least as wide", "[frame]") {
    UiDesignFrame frame{1920.0f, 1080.0f};
    REQUIRE(frame.scale_for(1920.0f, 1080.0f) == Catch::Approx(1.0f));
    REQUIRE(frame.scale_for(3840.0f, 2160.0f) == Catch::Approx(2.0f));
    REQUIRE(frame.scale_for(2560.0f, 1080.0f) == Catch::Approx(1.0f)); // ultrawide: height wins
}

TEST_CASE("Design frame scales by width on a narrower viewport", "[frame]") {
    UiDesignFrame frame{1920.0f, 1080.0f};
    REQUIRE(frame.scale_for(960.0f, 1080.0f) == Catch::Approx(0.5f));
    REQUIRE(frame.scale_for(1067.0f, 1430.0f) == Catch::Approx(1067.0f / 1920.0f));
}

TEST_CASE("A frame without a width scales by height alone", "[frame]") {
    UiDesignFrame frame{0.0f, 1080.0f};
    REQUIRE(frame.scale_for(960.0f, 1080.0f) == Catch::Approx(1.0f));
    REQUIRE(frame.scale_for(960.0f, 540.0f) == Catch::Approx(0.5f));
}

TEST_CASE("A frame without a height means native pixels", "[frame]") {
    UiDesignFrame frame{};
    REQUIRE(frame.scale_for(800.0f, 600.0f) == Catch::Approx(1.0f));
}

TEST_CASE("Layout rounds to whole screen pixels at the frame scale", "[frame]") {
    auto root = make_node("panel", {"root"});
    auto* child = add_child(*root, make_node("div", {"a"}));
    auto sheet = FjssParser::parse(".a { width: 10.3; height: 10.3; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    UiLayout layout;
    // Two screen pixels per layout pixel: results land on halves.
    layout.compute(*root, 400.0f, 300.0f, 2.0f);
    REQUIRE(child->layout_rect.w == Catch::Approx(10.5f));
}
