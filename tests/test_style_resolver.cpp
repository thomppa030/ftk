#include "ui/game_ui/style_resolver.hpp"
#include "ui/game_ui/fjss_parser.hpp"
#include "core/log.hpp"

#include "ui_length_printer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

using namespace fjell;

static struct LogInit {
    LogInit() { if (!log::core()) log::init(); }
} s_log_init;

// --- Helpers to build node trees without the FJUI parser ---

static std::unique_ptr<UiNode> make_node(const std::string& tag,
                                          const std::string& id = {},
                                          std::vector<std::string> classes = {}) {
    auto n = std::make_unique<UiNode>();
    n->type = UiNodeType::element;
    n->tag = tag;
    n->id = id;
    n->classes = std::move(classes);
    return n;
}

static UiNode* add_child(UiNode& parent, std::unique_ptr<UiNode> child) {
    child->parent = &parent;
    parent.children.push_back(std::move(child));
    return parent.children.back().get();
}

// --- Selector matching ---

TEST_CASE("Tag selector matches", "[style]") {
    auto root = make_node("div");
    auto sheet = FjssParser::parse("div { width: 100; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.width == UiLength::pixels(100.0f));
}

TEST_CASE("Tag selector doesn't match wrong tag", "[style]") {
    auto root = make_node("span");
    auto sheet = FjssParser::parse("div { width: 100; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.width.is_auto()); // default auto
}

TEST_CASE("Class selector matches", "[style]") {
    auto root = make_node("div", "", {"panel"});
    auto sheet = FjssParser::parse(".panel { width: 200; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.width == UiLength::pixels(200.0f));
}

TEST_CASE("Class selector doesn't match wrong class", "[style]") {
    auto root = make_node("div", "", {"other"});
    auto sheet = FjssParser::parse(".panel { width: 200; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.width.is_auto());
}

TEST_CASE("ID selector matches", "[style]") {
    auto root = make_node("div", "hud");
    auto sheet = FjssParser::parse("#hud { opacity: 0.5; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.opacity == Catch::Approx(0.5f));
}

TEST_CASE("Descendant selector matches ancestor", "[style]") {
    auto root = make_node("panel");
    auto* child = add_child(*root, make_node("div", "", {"label"}));

    auto sheet = FjssParser::parse("panel .label { width: 50; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(child->style.width == UiLength::pixels(50.0f));
}

TEST_CASE("Descendant selector doesn't match without ancestor", "[style]") {
    auto root = make_node("other");
    auto* child = add_child(*root, make_node("div", "", {"label"}));

    auto sheet = FjssParser::parse("panel .label { width: 50; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(child->style.width.is_auto());
}

TEST_CASE("Descendant selector matches deep ancestor", "[style]") {
    auto root = make_node("panel");
    auto* mid = add_child(*root, make_node("div"));
    auto* deep = add_child(*mid, make_node("span", "", {"label"}));

    auto sheet = FjssParser::parse("panel .label { width: 75; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(deep->style.width == UiLength::pixels(75.0f));
}

// --- Pseudo-class matching ---

TEST_CASE("Hover pseudo-class matches when hovered", "[style]") {
    auto root = make_node("div", "", {"btn"});
    root->interaction_state = 1; // hovered

    auto sheet = FjssParser::parse(".btn:hover { opacity: 0.8; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.opacity == Catch::Approx(0.8f));
}

TEST_CASE("Hover pseudo-class doesn't match when not hovered", "[style]") {
    auto root = make_node("div", "", {"btn"});
    root->interaction_state = 0;

    auto sheet = FjssParser::parse(".btn:hover { opacity: 0.8; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.opacity == Catch::Approx(1.0f)); // default
}

TEST_CASE("Active pseudo-class matches when active", "[style]") {
    auto root = make_node("div", "", {"btn"});
    root->interaction_state = 2; // active

    auto sheet = FjssParser::parse(".btn:active { opacity: 0.6; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.opacity == Catch::Approx(0.6f));
}

TEST_CASE("Focus pseudo-class matches when focused", "[style]") {
    auto root = make_node("div", "", {"input"});
    root->interaction_state = 4; // focused

    auto sheet = FjssParser::parse(".input:focus { border-width: 2; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.border_width == Catch::Approx(2.0f));
}

// --- Specificity ordering ---

TEST_CASE("Higher specificity overrides lower", "[style]") {
    auto root = make_node("div", "main", {"panel"});

    // tag(1) vs class(10) vs id(100) — all set width differently
    auto sheet = FjssParser::parse(R"(
        div { width: 10; }
        .panel { width: 20; }
        #main { width: 30; }
    )");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.width == UiLength::pixels(30.0f)); // id wins
}

TEST_CASE("Equal specificity: last rule wins", "[style]") {
    auto root = make_node("div", "", {"a", "b"});

    auto sheet = FjssParser::parse(R"(
        .a { width: 10; }
        .b { width: 20; }
    )");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    // Both have specificity 10. Sort is stable for equal specificity,
    // so .b (later in rules) is applied after .a.
    REQUIRE(root->style.width == UiLength::pixels(20.0f));
}

TEST_CASE("Pseudo-class adds specificity", "[style]") {
    auto root = make_node("div", "", {"btn"});
    root->interaction_state = 1; // hovered

    // .btn = 10, .btn:hover = 20
    auto sheet = FjssParser::parse(R"(
        .btn:hover { width: 200; }
        .btn { width: 100; }
    )");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    // .btn:hover(20) > .btn(10), so hover wins regardless of rule order
    REQUIRE(root->style.width == UiLength::pixels(200.0f));
}

// --- Inheritance ---

TEST_CASE("Color inherits from parent", "[style]") {
    auto root = make_node("div");
    auto* child = add_child(*root, make_node("span"));

    auto sheet = FjssParser::parse("div { color: #ff0000; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);

    REQUIRE(child->style.color.r == Catch::Approx(1.0f));
    REQUIRE(child->style.color.g == Catch::Approx(0.0f));
}

TEST_CASE("Font-size inherits from parent", "[style]") {
    auto root = make_node("div");
    auto* child = add_child(*root, make_node("span"));

    auto sheet = FjssParser::parse("div { font-size: 24; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(child->style.font_size == Catch::Approx(24.0f));
}

TEST_CASE("Opacity inherits from parent", "[style]") {
    auto root = make_node("div");
    auto* child = add_child(*root, make_node("span"));

    auto sheet = FjssParser::parse("div { opacity: 0.5; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(child->style.opacity == Catch::Approx(0.5f));
}

TEST_CASE("Width does NOT inherit", "[style]") {
    auto root = make_node("div");
    auto* child = add_child(*root, make_node("span"));

    auto sheet = FjssParser::parse("div { width: 100; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(child->style.width.is_auto()); // auto, not 100
}

TEST_CASE("Child rule overrides inherited value", "[style]") {
    auto root = make_node("div");
    auto* child = add_child(*root, make_node("span"));

    auto sheet = FjssParser::parse(R"(
        div { color: #ff0000; }
        span { color: #00ff00; }
    )");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(child->style.color.r == Catch::Approx(0.0f));
    REQUIRE(child->style.color.g == Catch::Approx(1.0f));
}

TEST_CASE("Inheritance chains through multiple levels", "[style]") {
    auto root = make_node("div");
    auto* mid = add_child(*root, make_node("span"));
    auto* deep = add_child(*mid, make_node("text"));

    auto sheet = FjssParser::parse("div { font-size: 32; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(deep->style.font_size == Catch::Approx(32.0f));
}

// --- Inline style-* attributes ---

TEST_CASE("Inline style-* overrides stylesheet", "[style]") {
    auto root = make_node("div", "", {"panel"});
    root->attributes["style-width"] = "999";

    auto sheet = FjssParser::parse(".panel { width: 100; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.width == UiLength::pixels(999.0f));
}

TEST_CASE("Inline style-* with no stylesheet", "[style]") {
    auto root = make_node("div");
    root->attributes["style-opacity"] = "0.3";

    Stylesheet empty;
    StyleResolver resolver;
    resolver.resolve(*root, empty);
    REQUIRE(root->style.opacity == Catch::Approx(0.3f));
}

// --- Color parsing ---

TEST_CASE("Parse #RRGGBB color", "[style]") {
    auto root = make_node("div");
    auto sheet = FjssParser::parse("div { color: #ff8040; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.color.r == Catch::Approx(1.0f));
    REQUIRE(root->style.color.g == Catch::Approx(0x80 / 255.0f));
    REQUIRE(root->style.color.b == Catch::Approx(0x40 / 255.0f));
    REQUIRE(root->style.color.a == Catch::Approx(1.0f));
}

TEST_CASE("Parse #RRGGBBAA color", "[style]") {
    auto root = make_node("div");
    auto sheet = FjssParser::parse("div { background-color: #ff000080; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.background_color.r == Catch::Approx(1.0f));
    REQUIRE(root->style.background_color.a == Catch::Approx(0x80 / 255.0f));
}

TEST_CASE("Parse #RGB shorthand color", "[style]") {
    auto root = make_node("div");
    auto sheet = FjssParser::parse("div { color: #f00; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.color.r == Catch::Approx(1.0f));
    REQUIRE(root->style.color.g == Catch::Approx(0.0f));
    REQUIRE(root->style.color.b == Catch::Approx(0.0f));
}

TEST_CASE("Parse named color", "[style]") {
    auto root = make_node("div");
    auto sheet = FjssParser::parse("div { color: white; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.color.r == Catch::Approx(1.0f));
    REQUIRE(root->style.color.g == Catch::Approx(1.0f));
    REQUIRE(root->style.color.b == Catch::Approx(1.0f));
}

TEST_CASE("Parse transparent color", "[style]") {
    auto root = make_node("div");
    auto sheet = FjssParser::parse("div { background-color: transparent; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.background_color.a == Catch::Approx(0.0f));
}

// --- Edge shorthand parsing ---

TEST_CASE("Margin shorthand: 1 value", "[style]") {
    auto root = make_node("div");
    auto sheet = FjssParser::parse("div { margin: 10; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.margin.top == UiLength::pixels(10.0f));
    REQUIRE(root->style.margin.right == UiLength::pixels(10.0f));
    REQUIRE(root->style.margin.bottom == UiLength::pixels(10.0f));
    REQUIRE(root->style.margin.left == UiLength::pixels(10.0f));
}

TEST_CASE("Margin shorthand: 2 values (vertical horizontal)", "[style]") {
    auto root = make_node("div");
    auto sheet = FjssParser::parse("div { margin: 10 20; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.margin.top == UiLength::pixels(10.0f));
    REQUIRE(root->style.margin.right == UiLength::pixels(20.0f));
    REQUIRE(root->style.margin.bottom == UiLength::pixels(10.0f));
    REQUIRE(root->style.margin.left == UiLength::pixels(20.0f));
}

TEST_CASE("Margin shorthand: 4 values", "[style]") {
    auto root = make_node("div");
    auto sheet = FjssParser::parse("div { margin: 1 2 3 4; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.margin.top == UiLength::pixels(1.0f));
    REQUIRE(root->style.margin.right == UiLength::pixels(2.0f));
    REQUIRE(root->style.margin.bottom == UiLength::pixels(3.0f));
    REQUIRE(root->style.margin.left == UiLength::pixels(4.0f));
}

TEST_CASE("Individual margin sides", "[style]") {
    auto root = make_node("div");
    auto sheet = FjssParser::parse("div { margin-top: 5; margin-left: 15; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.margin.top == UiLength::pixels(5.0f));
    REQUIRE(root->style.margin.left == UiLength::pixels(15.0f));
    REQUIRE(root->style.margin.right == UiLength::pixels(0.0f));
}

TEST_CASE("Border-radius shorthand: 1 value", "[style]") {
    auto root = make_node("div");
    auto sheet = FjssParser::parse("div { border-radius: 8; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.border_radius.top_left == Catch::Approx(8.0f));
    REQUIRE(root->style.border_radius.bottom_right == Catch::Approx(8.0f));
}

TEST_CASE("Border-radius shorthand: 4 values", "[style]") {
    auto root = make_node("div");
    auto sheet = FjssParser::parse("div { border-radius: 1 2 3 4; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.border_radius.top_left == Catch::Approx(1.0f));
    REQUIRE(root->style.border_radius.top_right == Catch::Approx(2.0f));
    REQUIRE(root->style.border_radius.bottom_right == Catch::Approx(3.0f));
    REQUIRE(root->style.border_radius.bottom_left == Catch::Approx(4.0f));
}

// --- Property application ---

TEST_CASE("Display property", "[style]") {
    auto root = make_node("div");
    auto sheet = FjssParser::parse("div { display: none; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.display == UiDisplay::none);
}

TEST_CASE("Flex-direction property", "[style]") {
    auto root = make_node("div");
    auto sheet = FjssParser::parse("div { flex-direction: row; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.flex_direction == UiFlexDirection::row);
}

TEST_CASE("Position absolute", "[style]") {
    auto root = make_node("div");
    auto sheet = FjssParser::parse("div { position: absolute; top: 10; left: 20; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.position == UiPosition::absolute);
    REQUIRE(root->style.inset.top == UiLength::pixels(10.0f));
    REQUIRE(root->style.inset.left == UiLength::pixels(20.0f));
}

TEST_CASE("Auto width parses as -1", "[style]") {
    auto root = make_node("div");
    auto sheet = FjssParser::parse("div { width: auto; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.width.is_auto());
}

TEST_CASE("Gap shorthand sets both row and column gap", "[style]") {
    auto root = make_node("div");
    auto sheet = FjssParser::parse("div { gap: 8; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.row_gap == UiLength::pixels(8.0f));
    REQUIRE(root->style.column_gap == UiLength::pixels(8.0f));
}

TEST_CASE("Border shorthand parses width and color", "[style]") {
    auto root = make_node("div");
    auto sheet = FjssParser::parse("div { border: 2 solid #ff0000; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.border_width == Catch::Approx(2.0f));
    REQUIRE(root->style.border_color.r == Catch::Approx(1.0f));
}

TEST_CASE("Transition property is stored as string", "[style]") {
    auto root = make_node("div");
    auto sheet = FjssParser::parse("div { transition: all 0.3s ease-in-out; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.transition == "all 0.3s ease-in-out");
}

// --- resolve_subtree ---

TEST_CASE("resolve_subtree re-applies rules with updated state", "[style]") {
    auto root = make_node("div", "", {"btn"});
    auto sheet = FjssParser::parse(R"(
        .btn { opacity: 1; }
        .btn:hover { opacity: 0.8; }
    )");

    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.opacity == Catch::Approx(1.0f));

    // Simulate hover
    root->interaction_state = 1;
    resolver.resolve_subtree(*root, sheet);
    REQUIRE(root->style.opacity == Catch::Approx(0.8f));

    // Simulate unhover
    root->interaction_state = 0;
    resolver.resolve_subtree(*root, sheet);
    REQUIRE(root->style.opacity == Catch::Approx(1.0f));
}

TEST_CASE("resolve_subtree inherits from parent", "[style]") {
    auto root = make_node("div");
    auto* child = add_child(*root, make_node("span"));

    auto sheet = FjssParser::parse("div { font-size: 24; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);

    // Re-resolve child — should still inherit font-size
    resolver.resolve_subtree(*child, sheet);
    REQUIRE(child->style.font_size == Catch::Approx(24.0f));
}

// --- Complex real-world scenario ---

TEST_CASE("Full cascade: tag + class + id + pseudo + inheritance + inline", "[style]") {
    auto root = make_node("panel", "hud");
    auto* slot = add_child(*root, make_node("div", "", {"slot"}));
    slot->interaction_state = 1; // hovered
    slot->attributes["style-border-radius"] = "12";

    auto sheet = FjssParser::parse(R"(
        panel { color: #ffffff; font-size: 14; }
        .slot { width: 48; height: 48; background-color: #222222; border: 2 solid #555555; border-radius: 6; }
        .slot:hover { border-color: #888888; }
        #hud { opacity: 0.9; }
    )");

    StyleResolver resolver;
    resolver.resolve(*root, sheet);

    // Root: id rule sets opacity
    REQUIRE(root->style.opacity == Catch::Approx(0.9f));

    // Slot: class rule sets dimensions + border
    REQUIRE(slot->style.width == UiLength::pixels(48.0f));
    REQUIRE(slot->style.height == UiLength::pixels(48.0f));
    REQUIRE(slot->style.border_width == Catch::Approx(2.0f));

    // Slot: hover pseudo overrides border-color
    REQUIRE(slot->style.border_color.r == Catch::Approx(0x88 / 255.0f));

    // Slot: inline style-* overrides stylesheet border-radius
    REQUIRE(slot->style.border_radius.top_left == Catch::Approx(12.0f));

    // Slot: inherits color and font-size from panel
    REQUIRE(slot->style.color.r == Catch::Approx(1.0f));
    REQUIRE(slot->style.font_size == Catch::Approx(14.0f));

    // Slot: inherits opacity from parent (which was set by #hud rule)
    REQUIRE(slot->style.opacity == Catch::Approx(0.9f));
}

// --- Empty / edge cases ---

TEST_CASE("Empty stylesheet leaves defaults", "[style]") {
    auto root = make_node("div");
    Stylesheet empty;
    StyleResolver resolver;
    resolver.resolve(*root, empty);
    REQUIRE(root->style.width.is_auto());
    REQUIRE(root->style.opacity == Catch::Approx(1.0f));
    REQUIRE(root->style.display == UiDisplay::flex);
}

TEST_CASE("No matching rules leaves defaults", "[style]") {
    auto root = make_node("div");
    auto sheet = FjssParser::parse(".nonexistent { width: 999; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.width.is_auto());
}

// --- Stability & ordering edge cases ---

TEST_CASE("Equal specificity: rule order must be preserved (stable sort)", "[style][edge]") {
    // If std::sort is used instead of std::stable_sort, equal-specificity
    // rules can be reordered. With many rules this becomes likely.
    auto root = make_node("div", "", {"x"});

    // 50 class rules, all specificity 10, each setting width differently.
    // The LAST one must win. std::sort may reorder equal elements.
    std::string css;
    for (int i = 0; i < 50; i++) {
        css += ".x { width: " + std::to_string(i) + "; }\n";
    }
    auto sheet = FjssParser::parse(css);
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.width == UiLength::pixels(49.0f));
}

TEST_CASE("Equal specificity with interleaved properties: last per-property wins", "[style][edge]") {
    // Two rules with same specificity set overlapping properties.
    // Even if sort reorders them, each property should get the value
    // from the last rule that sets it in stylesheet order.
    auto root = make_node("div", "", {"a"});

    auto sheet = FjssParser::parse(R"(
        .a { width: 10; height: 100; }
        .a { width: 20; opacity: 0.5; }
    )");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);

    // width: second rule (20) should win over first (10)
    REQUIRE(root->style.width == UiLength::pixels(20.0f));
    // height: only first rule sets it
    REQUIRE(root->style.height == UiLength::pixels(100.0f));
    // opacity: only second rule sets it
    REQUIRE(root->style.opacity == Catch::Approx(0.5f));
}

TEST_CASE("Tag+class selector requires both to match", "[style][edge]") {
    // "div.panel" should NOT match <span class="panel">
    auto span_panel = make_node("span", "", {"panel"});
    auto div_panel = make_node("div", "", {"panel"});
    auto div_only = make_node("div");

    auto sheet = FjssParser::parse("div.panel { width: 42; }");
    StyleResolver resolver;

    resolver.resolve(*span_panel, sheet);
    REQUIRE(span_panel->style.width.is_auto()); // no match

    resolver.resolve(*div_panel, sheet);
    REQUIRE(div_panel->style.width == UiLength::pixels(42.0f)); // match

    resolver.resolve(*div_only, sheet);
    REQUIRE(div_only->style.width.is_auto()); // no match (no class)
}

TEST_CASE("Bare ancestor name is a tag, not a class or id", "[style][selector]") {
    // "panel .label" — the ancestor must be a <panel>, exactly as in CSS.
    auto by_class = make_node("div", "", {"panel"});
    auto* c1 = add_child(*by_class, make_node("span", "", {"label"}));
    auto by_id = make_node("div", "panel");
    auto* c2 = add_child(*by_id, make_node("span", "", {"label"}));

    auto sheet = FjssParser::parse("panel .label { width: 77; }");
    StyleResolver resolver;
    resolver.resolve(*by_class, sheet);
    resolver.resolve(*by_id, sheet);
    REQUIRE(c1->style.width.is_auto());
    REQUIRE(c2->style.width.is_auto());
}

TEST_CASE("Class and id ancestors match their own kind", "[style][selector]") {
    auto root = make_node("div", "hud", {"panel"});
    auto* child = add_child(*root, make_node("span", "", {"label"}));
    StyleResolver resolver;

    resolver.resolve(*root, FjssParser::parse(".panel .label { width: 1; }"));
    REQUIRE(child->style.width == UiLength::pixels(1.0f));
    resolver.resolve(*root, FjssParser::parse("#hud .label { width: 2; }"));
    REQUIRE(child->style.width == UiLength::pixels(2.0f));
    resolver.resolve(*root, FjssParser::parse(".other .label { width: 3; }"));
    REQUIRE(child->style.width.is_auto());
}

TEST_CASE("Multi-class compound needs every class and outranks a single one", "[style][selector]") {
    auto both = make_node("div", "", {"vital-fill", "blue"});
    auto one = make_node("div", "", {"blue"});
    auto sheet = FjssParser::parse(R"(
        .vital-fill.blue { width: 1; }
        .blue { width: 2; }
    )");
    StyleResolver resolver;
    resolver.resolve(*both, sheet);
    resolver.resolve(*one, sheet);
    // Two classes beat one even though the single-class rule comes later
    REQUIRE(both->style.width == UiLength::pixels(1.0f));
    REQUIRE(one->style.width == UiLength::pixels(2.0f));
}

TEST_CASE("Child combinator matches only the direct parent", "[style][selector]") {
    auto root = make_node("div", "", {"a"});
    auto* mid = add_child(*root, make_node("div"));
    auto* direct = add_child(*root, make_node("div", "", {"b"}));
    auto* grand = add_child(*mid, make_node("div", "", {"b"}));

    auto sheet = FjssParser::parse(".a > .b { width: 5; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(direct->style.width == UiLength::pixels(5.0f));
    REQUIRE(grand->style.width.is_auto());
}

TEST_CASE("Every step of a deep chain must be present", "[style][selector]") {
    auto root = make_node("div", "", {"a"});
    auto* b = add_child(*root, make_node("div", "", {"b"}));
    auto* c_under_b = add_child(*b, make_node("div", "", {"c"}));
    auto* c_under_a = add_child(*root, make_node("div", "", {"c"}));

    auto sheet = FjssParser::parse(".a .b .c { width: 9; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(c_under_b->style.width == UiLength::pixels(9.0f));
    REQUIRE(c_under_a->style.width.is_auto());
}

TEST_CASE("Mixed combinators backtrack past the nearest candidate", "[style][selector]") {
    // .a > .b .c must match when the nearest .b above .c is not a child of
    // .a, but a farther .b is: greedy matching from the inside would miss it.
    auto a = make_node("div", "", {"a"});
    auto* outer_b = add_child(*a, make_node("div", "", {"b"}));
    auto* inner_b = add_child(*outer_b, make_node("div", "", {"b"}));
    auto* c = add_child(*inner_b, make_node("div", "", {"c"}));

    auto sheet = FjssParser::parse(".a > .b .c { width: 4; }");
    StyleResolver resolver;
    resolver.resolve(*a, sheet);
    REQUIRE(c->style.width == UiLength::pixels(4.0f));
}

TEST_CASE("Calling resolve resets and reapplies from scratch", "[style][edge]") {
    auto root = make_node("div");

    auto sheet1 = FjssParser::parse("div { width: 100; }");
    auto sheet2 = FjssParser::parse("div { height: 200; }");

    StyleResolver resolver;
    resolver.resolve(*root, sheet1);
    REQUIRE(root->style.width == UiLength::pixels(100.0f));
    REQUIRE(root->style.height.is_auto());

    resolver.resolve(*root, sheet2);
    // width from first resolve is reset (styles start fresh each resolve)
    REQUIRE(root->style.width.is_auto());
    REQUIRE(root->style.height == UiLength::pixels(200.0f));
}

TEST_CASE("resolve_subtree resets non-inherited properties", "[style][edge]") {
    auto root = make_node("div", "", {"box"});

    auto sheet = FjssParser::parse(".box { width: 100; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.width == UiLength::pixels(100.0f));

    // Re-resolve with empty sheet — should reset width to default
    Stylesheet empty;
    resolver.resolve_subtree(*root, empty);
    REQUIRE(root->style.width.is_auto());
}

TEST_CASE("Hover rule doesn't leak into non-hovered sibling", "[style][edge]") {
    auto root = make_node("div");
    auto* a = add_child(*root, make_node("span", "", {"btn"}));
    auto* b = add_child(*root, make_node("span", "", {"btn"}));
    a->interaction_state = 1; // hovered
    b->interaction_state = 0; // not hovered

    auto sheet = FjssParser::parse(R"(
        .btn { opacity: 1.0; }
        .btn:hover { opacity: 0.5; }
    )");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);

    REQUIRE(a->style.opacity == Catch::Approx(0.5f));
    REQUIRE(b->style.opacity == Catch::Approx(1.0f));
}

TEST_CASE("Inherited property overridden by child, then child re-resolved", "[style][edge]") {
    auto root = make_node("div");
    auto* child = add_child(*root, make_node("span"));

    auto sheet = FjssParser::parse(R"(
        div { color: #ff0000; }
        span { color: #00ff00; }
    )");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);

    // Child has green from its own rule
    REQUIRE(child->style.color.g == Catch::Approx(1.0f));

    // Re-resolve child with same sheet — should still be green, not red
    resolver.resolve_subtree(*child, sheet);
    REQUIRE(child->style.color.g == Catch::Approx(1.0f));
    REQUIRE(child->style.color.r == Catch::Approx(0.0f));
}

TEST_CASE("Multiple classes: rules for each class both apply", "[style][edge]") {
    auto root = make_node("div", "", {"wide", "tall"});

    auto sheet = FjssParser::parse(R"(
        .wide { width: 200; }
        .tall { height: 300; }
    )");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);

    REQUIRE(root->style.width == UiLength::pixels(200.0f));
    REQUIRE(root->style.height == UiLength::pixels(300.0f));
}

TEST_CASE("Multiple classes: conflicting rules, last class rule wins", "[style][edge]") {
    auto root = make_node("div", "", {"red", "blue"});

    auto sheet = FjssParser::parse(R"(
        .red { background-color: #ff0000; }
        .blue { background-color: #0000ff; }
    )");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);

    // Both match, same specificity (10), .blue is later — should win
    REQUIRE(root->style.background_color.b == Catch::Approx(1.0f));
    REQUIRE(root->style.background_color.r == Catch::Approx(0.0f));
}

// --- Gradient parsing ---

TEST_CASE("Linear gradient with angle and two colors", "[style][edge]") {
    auto root = make_node("div");
    auto sheet = FjssParser::parse("div { background: linear-gradient(90deg, #ff0000, #0000ff); }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.gradient.angle == Catch::Approx(90.0f));
    REQUIRE(root->style.gradient.from.r == Catch::Approx(1.0f));
    REQUIRE(root->style.gradient.to.b == Catch::Approx(1.0f));
    REQUIRE(root->style.gradient.active());
}

TEST_CASE("Linear gradient with 'to right' direction", "[style][edge]") {
    auto root = make_node("div");
    auto sheet = FjssParser::parse("div { background: linear-gradient(to right, #000, #fff); }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.gradient.angle == Catch::Approx(90.0f));
}

TEST_CASE("Linear gradient with two colors only (no angle)", "[style][edge]") {
    auto root = make_node("div");
    auto sheet = FjssParser::parse("div { background: linear-gradient(#ff0000, #00ff00); }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.gradient.angle == Catch::Approx(180.0f)); // default
    REQUIRE(root->style.gradient.from.r == Catch::Approx(1.0f));
    REQUIRE(root->style.gradient.to.g == Catch::Approx(1.0f));
}

// --- Box shadow parsing ---

TEST_CASE("Box shadow with hex color", "[style][edge]") {
    auto root = make_node("div");
    auto sheet = FjssParser::parse("div { box-shadow: 2 4 8 #000000; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.box_shadow.offset_x == Catch::Approx(2.0f));
    REQUIRE(root->style.box_shadow.offset_y == Catch::Approx(4.0f));
    REQUIRE(root->style.box_shadow.blur == Catch::Approx(8.0f));
    REQUIRE(root->style.box_shadow.color.a == Catch::Approx(1.0f));
}

TEST_CASE("Box shadow with spread and hex color", "[style][edge]") {
    auto root = make_node("div");
    auto sheet = FjssParser::parse("div { box-shadow: 0 2 4 1 #00000080; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.box_shadow.spread == Catch::Approx(1.0f));
    REQUIRE(root->style.box_shadow.color.a == Catch::Approx(0x80 / 255.0f));
}

TEST_CASE("Box shadow none resets", "[style][edge]") {
    auto root = make_node("div");
    auto sheet = FjssParser::parse(R"(
        div { box-shadow: 0 4 8 #000; }
    )");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.box_shadow.blur == Catch::Approx(8.0f));

    // Now apply "none"
    auto sheet2 = FjssParser::parse("div { box-shadow: none; }");
    resolver.resolve(*root, sheet2);
    REQUIRE(root->style.box_shadow.blur == Catch::Approx(0.0f));
}

// --- Color parsing edge cases ---

TEST_CASE("rgba() color parsing", "[style][edge]") {
    auto root = make_node("div");
    auto sheet = FjssParser::parse("div { color: rgba(255, 128, 0, 0.5); }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.color.r == Catch::Approx(1.0f));
    REQUIRE(root->style.color.g == Catch::Approx(128.0f / 255.0f));
    REQUIRE(root->style.color.b == Catch::Approx(0.0f));
    REQUIRE(root->style.color.a == Catch::Approx(0.5f));
}

TEST_CASE("rgb() color parsing", "[style][edge]") {
    auto root = make_node("div");
    auto sheet = FjssParser::parse("div { color: rgb(0, 255, 128); }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.color.r == Catch::Approx(0.0f));
    REQUIRE(root->style.color.g == Catch::Approx(1.0f));
    REQUIRE(root->style.color.b == Catch::Approx(128.0f / 255.0f));
    REQUIRE(root->style.color.a == Catch::Approx(1.0f));
}

TEST_CASE("All named colors parse", "[style][edge]") {
    StyleResolver resolver;
    auto test_color = [&](const std::string& name) {
        auto root = make_node("div");
        auto sheet = FjssParser::parse("div { color: " + name + "; }");
        resolver.resolve(*root, sheet);
        return root->style.color;
    };

    REQUIRE(test_color("red").r == Catch::Approx(1.0f));
    REQUIRE(test_color("green").g == Catch::Approx(0.5f)); // CSS green is 0,128,0
    REQUIRE(test_color("blue").b == Catch::Approx(1.0f));
    REQUIRE(test_color("yellow").r == Catch::Approx(1.0f));
    REQUIRE(test_color("yellow").g == Catch::Approx(1.0f));
    REQUIRE(test_color("cyan").g == Catch::Approx(1.0f));
    REQUIRE(test_color("magenta").r == Catch::Approx(1.0f));
    REQUIRE(test_color("gray").r == Catch::Approx(0.5f));
    REQUIRE(test_color("grey").r == Catch::Approx(0.5f)); // british spelling
    REQUIRE(test_color("orange").r == Catch::Approx(1.0f));
    REQUIRE(test_color("black").r == Catch::Approx(0.0f));
    REQUIRE(test_color("black").a == Catch::Approx(1.0f));
}

TEST_CASE("Unknown color name defaults to opaque black", "[style][edge]") {
    auto root = make_node("div");
    auto sheet = FjssParser::parse("div { color: nonexistent; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.color.r == Catch::Approx(0.0f));
    REQUIRE(root->style.color.g == Catch::Approx(0.0f));
    REQUIRE(root->style.color.b == Catch::Approx(0.0f));
    REQUIRE(root->style.color.a == Catch::Approx(1.0f)); // UiColor default alpha
}

// --- Edges shorthand: 3 values ---

TEST_CASE("Padding shorthand: 3 values (top, horizontal, bottom)", "[style][edge]") {
    auto root = make_node("div");
    auto sheet = FjssParser::parse("div { padding: 10 20 30; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.padding.top == UiLength::pixels(10.0f));
    REQUIRE(root->style.padding.right == UiLength::pixels(20.0f));
    REQUIRE(root->style.padding.bottom == UiLength::pixels(30.0f));
    REQUIRE(root->style.padding.left == UiLength::pixels(20.0f)); // same as right
}

// --- Overflow / flex properties ---

TEST_CASE("Overflow scroll", "[style][edge]") {
    auto root = make_node("div");
    auto sheet = FjssParser::parse("div { overflow: scroll; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.overflow == UiOverflow::scroll);
}

TEST_CASE("Flex grow and shrink", "[style][edge]") {
    auto root = make_node("div");
    auto sheet = FjssParser::parse("div { flex-grow: 1; flex-shrink: 0; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.flex_grow == Catch::Approx(1.0f));
    REQUIRE(root->style.flex_shrink == Catch::Approx(0.0f));
}

TEST_CASE("Justify-content space-evenly", "[style][edge]") {
    auto root = make_node("div");
    auto sheet = FjssParser::parse("div { justify-content: space-evenly; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.justify_content == UiJustify::space_evenly);
}

TEST_CASE("Align-items baseline", "[style][edge]") {
    auto root = make_node("div");
    auto sheet = FjssParser::parse("div { align-items: baseline; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.align_items == UiAlign::baseline);
}

TEST_CASE("Flex-wrap wrap-reverse", "[style][edge]") {
    auto root = make_node("div");
    auto sheet = FjssParser::parse("div { flex-wrap: wrap-reverse; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.flex_wrap == UiFlexWrap::wrap_reverse);
}

TEST_CASE("Min/max width and height", "[style][edge]") {
    auto root = make_node("div");
    auto sheet = FjssParser::parse("div { min-width: 50; max-width: 300; min-height: 20; max-height: 400; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.min_width == UiLength::pixels(50.0f));
    REQUIRE(root->style.max_width == UiLength::pixels(300.0f));
    REQUIRE(root->style.min_height == UiLength::pixels(20.0f));
    REQUIRE(root->style.max_height == UiLength::pixels(400.0f));
}

// --- Selector: combined hovered + active ---

TEST_CASE("Hovered AND active simultaneously", "[style][edge]") {
    auto root = make_node("div", "", {"btn"});
    root->interaction_state = 1 | 2; // hovered + active

    auto sheet = FjssParser::parse(R"(
        .btn { opacity: 1.0; }
        .btn:hover { opacity: 0.8; }
        .btn:active { opacity: 0.6; }
    )");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    // :active(specificity 20) == :hover(specificity 20), but :active is later → wins
    REQUIRE(root->style.opacity == Catch::Approx(0.6f));
}

TEST_CASE("Selector with tag + class + id all required", "[style][edge]") {
    // tag(div) + class(panel) + id(main) selector parsed from "div.panel#main"
    // The FJSS parser may not support this combo — let's test what happens
    auto sheet = FjssParser::parse("div.panel#main { width: 42; }");
    REQUIRE(sheet.rules.size() == 1);

    auto matching = make_node("div", "main", {"panel"});
    auto wrong_tag = make_node("span", "main", {"panel"});
    auto wrong_class = make_node("div", "main", {"other"});
    auto wrong_id = make_node("div", "wrong", {"panel"});

    StyleResolver resolver;
    resolver.resolve(*matching, sheet);
    REQUIRE(matching->style.width == UiLength::pixels(42.0f));

    resolver.resolve(*wrong_tag, sheet);
    REQUIRE(wrong_tag->style.width.is_auto());

    resolver.resolve(*wrong_class, sheet);
    REQUIRE(wrong_class->style.width.is_auto());

    resolver.resolve(*wrong_id, sheet);
    REQUIRE(wrong_id->style.width.is_auto());
}

TEST_CASE("Many rules resolved correctly", "[style]") {
    auto root = make_node("div", "", {"a", "b", "c"});

    std::string css;
    css += ".a { width: 10; }\n";
    css += ".b { height: 20; }\n";
    css += ".c { opacity: 0.5; }\n";

    auto sheet = FjssParser::parse(css);
    StyleResolver resolver;
    resolver.resolve(*root, sheet);

    REQUIRE(root->style.width == UiLength::pixels(10.0f));
    REQUIRE(root->style.height == UiLength::pixels(20.0f));
    REQUIRE(root->style.opacity == Catch::Approx(0.5f));
}

// --- Declaration order within a rule ---

TEST_CASE("Longhand after shorthand overrides one edge", "[style][order]") {
    auto root = make_node("div", "", {"a"});
    auto sheet = FjssParser::parse(".a { margin: 10; margin-left: 20; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.margin.left == UiLength::pixels(20.0f));
    REQUIRE(root->style.margin.top == UiLength::pixels(10.0f));
}

TEST_CASE("Shorthand after longhand resets the edge", "[style][order]") {
    auto root = make_node("div", "", {"a"});
    auto sheet = FjssParser::parse(".a { margin-left: 20; margin: 10; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.margin.left == UiLength::pixels(10.0f));
}

TEST_CASE("Border shorthand and border-color apply in authored order", "[style][order]") {
    auto root = make_node("div", "", {"a"});
    auto sheet = FjssParser::parse(".a { border-color: #ff0000; border: 2 solid #0000ff; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.border_color.b == Catch::Approx(1.0f));
    REQUIRE(root->style.border_color.r == Catch::Approx(0.0f));
}

TEST_CASE("resolve_subtree re-resolves descendants so inherited values follow", "[style]") {
    auto root = make_node("div", "", {"card"});
    auto* child = add_child(*root, make_node("text"));
    auto sheet = FjssParser::parse(".card { color: #000000; } .card:hover { color: #ff0000; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(child->style.color.r == Catch::Approx(0.0f));

    root->interaction_state |= 1; // hovered
    resolver.resolve_subtree(*root, sheet);
    REQUIRE(root->style.color.r == Catch::Approx(1.0f));
    // The text has no rule of its own; its color is inherited and must
    // track the parent's hover state, not the style from the last full resolve.
    REQUIRE(child->style.color.r == Catch::Approx(1.0f));
}

// --- Units ---

TEST_CASE("Percent, px and auto parse into lengths", "[style][units]") {
    auto root = make_node("div", "", {"a"});
    auto sheet = FjssParser::parse(".a { width: 50%; height: 24px; min-width: 10; margin: auto 4 2% 0; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.width == UiLength::percent(50.0f));
    REQUIRE(root->style.height == UiLength::pixels(24.0f));
    REQUIRE(root->style.min_width == UiLength::pixels(10.0f));
    REQUIRE(root->style.max_height.is_auto());
    REQUIRE(root->style.margin.top.is_auto());
    REQUIRE(root->style.margin.right == UiLength::pixels(4.0f));
    REQUIRE(root->style.margin.bottom == UiLength::percent(2.0f));
    REQUIRE(root->style.margin.left == UiLength::pixels(0.0f));
}

TEST_CASE("auto on a property without auto leaves it unset", "[style][units]") {
    auto root = make_node("div", "", {"a"});
    auto sheet = FjssParser::parse(".a { min-width: 10; min-width: auto; padding: auto; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.min_width == UiLength::pixels(10.0f));
    REQUIRE(root->style.padding.top == UiLength::pixels(0.0f));
}

TEST_CASE("top: 0 is a real inset, unset stays auto", "[style][units]") {
    auto root = make_node("div", "", {"a"});
    auto sheet = FjssParser::parse(".a { position: absolute; top: 0; left: 8; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.inset.top == UiLength::pixels(0.0f));
    REQUIRE(root->style.inset.left == UiLength::pixels(8.0f));
    REQUIRE(root->style.inset.right.is_auto());
    REQUIRE(root->style.inset.bottom.is_auto());
}

TEST_CASE("gap accepts a percentage", "[style][units]") {
    auto root = make_node("div", "", {"a"});
    auto sheet = FjssParser::parse(".a { gap: 5%; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.row_gap == UiLength::percent(5.0f));
    REQUIRE(root->style.column_gap == UiLength::percent(5.0f));
}

TEST_CASE("Lengths round-trip through to_css and parse_length", "[style][units]") {
    for (UiLength length : {UiLength::pixels(16.0f), UiLength::pixels(0.5f), UiLength::pixels(-3.0f),
                            UiLength::percent(50.0f), UiLength::percent(12.5f), UiLength::automatic()}) {
        INFO(to_css(length));
        REQUIRE(StyleResolver::parse_length(to_css(length)) == length);
    }
    // Pixels are written the way the sheets are authored: a bare number.
    REQUIRE(to_css(UiLength::pixels(240.0f)) == "240");
    REQUIRE(to_css(UiLength::percent(50.0f)) == "50%");
    REQUIRE(to_css(UiLength::automatic()) == "auto");
}

// --- Relative units ---

TEST_CASE("Relative units parse into their own unit", "[style][units]") {
    auto root = make_node("div", "", {"a"});
    auto sheet = FjssParser::parse(".a { width: 10vw; height: 2em; margin-top: 1.5rem; padding-left: 3vh; }");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.width == UiLength{10.0f, UiUnit::vw});
    REQUIRE(root->style.height == UiLength{2.0f, UiUnit::em});
    REQUIRE(root->style.margin.top == UiLength{1.5f, UiUnit::rem});
    REQUIRE(root->style.padding.left == UiLength{3.0f, UiUnit::vh});
}

TEST_CASE("font-size in em and percent builds on the parent", "[style][units]") {
    auto root = make_node("div", "", {"root"});
    auto* child = add_child(*root, make_node("div", "", {"child"}));
    auto* grandchild = add_child(*child, make_node("div", "", {"grandchild"}));
    auto sheet = FjssParser::parse(R"(
        .root { font-size: 20; }
        .child { font-size: 1.5em; }
        .grandchild { font-size: 50%; }
    )");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.font_size == Catch::Approx(20.0f));
    REQUIRE(child->style.font_size == Catch::Approx(30.0f));
    REQUIRE(grandchild->style.font_size == Catch::Approx(15.0f));
}

TEST_CASE("font-size in rem builds on the root, wherever the node sits", "[style][units]") {
    auto root = make_node("div", "", {"root"});
    auto* child = add_child(*root, make_node("div", "", {"child"}));
    auto* grandchild = add_child(*child, make_node("div", "", {"grandchild"}));
    auto sheet = FjssParser::parse(R"(
        .root { font-size: 10; }
        .child { font-size: 30; }
        .grandchild { font-size: 2rem; }
    )");
    StyleResolver resolver;
    resolver.resolve(*root, sheet);
    REQUIRE(grandchild->style.font_size == Catch::Approx(20.0f));
}

TEST_CASE("font-size in vh follows the viewport the resolver was given", "[style][units]") {
    auto root = make_node("div", "", {"a"});
    auto sheet = FjssParser::parse(".a { font-size: 5vh; }");
    StyleResolver resolver;
    resolver.set_viewport(800.0f, 600.0f);
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.font_size == Catch::Approx(30.0f));
    resolver.set_viewport(800.0f, 1000.0f);
    resolver.resolve(*root, sheet);
    REQUIRE(root->style.font_size == Catch::Approx(50.0f));
}

TEST_CASE("Relative lengths round-trip through to_css", "[style][units]") {
    for (UiLength length : {UiLength{1.5f, UiUnit::em}, UiLength{2.0f, UiUnit::rem},
                            UiLength{10.0f, UiUnit::vw}, UiLength{5.0f, UiUnit::vh},
                            UiLength{1.0f, UiUnit::vmin}, UiLength{1.0f, UiUnit::vmax}}) {
        INFO(to_css(length));
        REQUIRE(StyleResolver::parse_length(to_css(length)) == length);
    }
    REQUIRE(to_css(UiLength{1.5f, UiUnit::em}) == "1.5em");
}

// --- Explaining the cascade ---

TEST_CASE("explain names the winning rule per property", "[style][explain]") {
    auto root = make_node("div", "hud", {"panel"});
    auto* child = add_child(*root, make_node("span", "", {"label"}));
    auto sheet = FjssParser::parse(R"(
        .label { width: 10; color: red; }
        #hud .label { width: 20; }
        .panel span { height: 5; }
    )", "hud.fjss");

    auto origins = StyleResolver::explain(*child, sheet);
    REQUIRE(origins.size() == 3);
    REQUIRE(to_css(origins.at("width").rule->selector) == "#hud .label");
    REQUIRE(origins.at("width").declaration->value == "20");
    REQUIRE(to_css(origins.at("color").rule->selector) == ".label");
    REQUIRE(to_css(origins.at("height").rule->selector) == ".panel span");
    REQUIRE(origins.at("height").rule->source == "hud.fjss");
    // Inherited or default properties are not listed
    REQUIRE(origins.count("font-size") == 0);
}

TEST_CASE("explain reports inline style attributes over any rule", "[style][explain]") {
    auto root = make_node("div", "", {"a"});
    root->attributes["style-width"] = "42";
    auto sheet = FjssParser::parse(".a { width: 10; }");
    auto origins = StyleResolver::explain(*root, sheet);
    REQUIRE(origins.at("width").inline_style);
    REQUIRE(origins.at("width").rule == nullptr);
}

TEST_CASE("Selectors print the way they are written", "[style][explain]") {
    auto sheet = FjssParser::parse("panel > .slot:hover text, #a.b.c { width: 1; }");
    REQUIRE(to_css(sheet.rules[0].selector) == "panel > .slot:hover text");
    REQUIRE(to_css(sheet.rules[1].selector) == "#a.b.c");
}
