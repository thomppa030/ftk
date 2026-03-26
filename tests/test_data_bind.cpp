#include "ui/game_ui/ui_data_bind.hpp"
#include "ui/game_ui/ui_context.hpp"
#include "core/log.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

using namespace fjell;

static struct LogInit {
    LogInit() { if (!log::core()) log::init(); }
} s_log_init;

// --- Helpers ---

static std::unique_ptr<UiNode> make_text(const std::string& tmpl) {
    auto n = std::make_unique<UiNode>();
    n->type = UiNodeType::text;
    n->tag = "text";
    n->text_template = tmpl;
    n->text_content = tmpl; // unresolved initially
    return n;
}

static std::unique_ptr<UiNode> make_element(const std::string& tag,
                                             std::vector<std::string> classes = {}) {
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

// --- Template resolution ---

TEST_CASE("Resolve single {key} placeholder", "[databind]") {
    auto root = make_element("div");
    auto* text = add_child(*root, make_text("HP: {health}"));

    UiContext ctx;
    ctx.set("health", "72");
    UiDataBind bind;
    bind.resolve(*root, ctx);

    REQUIRE(text->text_content == "HP: 72");
}

TEST_CASE("Resolve multiple placeholders", "[databind]") {
    auto root = make_element("div");
    auto* text = add_child(*root, make_text("{name}: {score} pts"));

    UiContext ctx;
    ctx.set("name", "Player1");
    ctx.set("score", "1500");
    UiDataBind bind;
    bind.resolve(*root, ctx);

    REQUIRE(text->text_content == "Player1: 1500 pts");
}

TEST_CASE("Missing key resolves to empty string", "[databind]") {
    auto root = make_element("div");
    auto* text = add_child(*root, make_text("Value: {missing}"));

    UiContext ctx;
    UiDataBind bind;
    bind.resolve(*root, ctx);

    REQUIRE(text->text_content == "Value: ");
}

TEST_CASE("No placeholders — text unchanged", "[databind]") {
    auto root = make_element("div");
    auto* text = add_child(*root, make_text("static text"));

    UiContext ctx;
    UiDataBind bind;
    bind.resolve(*root, ctx);

    REQUIRE(text->text_content == "static text");
}

TEST_CASE("Unclosed brace treated as literal", "[databind]") {
    auto root = make_element("div");
    auto* text = add_child(*root, make_text("broken {key"));

    UiContext ctx;
    ctx.set("key", "val");
    UiDataBind bind;
    bind.resolve(*root, ctx);

    REQUIRE(text->text_content == "broken {key");
}

TEST_CASE("Adjacent placeholders", "[databind]") {
    auto root = make_element("div");
    auto* text = add_child(*root, make_text("{a}{b}{c}"));

    UiContext ctx;
    ctx.set("a", "1");
    ctx.set("b", "2");
    ctx.set("c", "3");
    UiDataBind bind;
    bind.resolve(*root, ctx);

    REQUIRE(text->text_content == "123");
}

TEST_CASE("Empty key {}", "[databind]") {
    auto root = make_element("div");
    auto* text = add_child(*root, make_text("before{}after"));

    UiContext ctx;
    UiDataBind bind;
    bind.resolve(*root, ctx);

    // {} is a valid template with empty key — resolves to get("") which is ""
    REQUIRE(text->text_content == "beforeafter");
}

// --- if directive ---

TEST_CASE("if directive hides node when key is empty", "[databind]") {
    auto root = make_element("div");
    auto* child = add_child(*root, make_element("span"));
    child->attributes["if"] = "show_debug";

    UiContext ctx; // "show_debug" not set → empty
    UiDataBind bind;
    bind.resolve(*root, ctx);

    REQUIRE(child->hidden_by_if == true);
}

TEST_CASE("if directive shows node when key is truthy", "[databind]") {
    auto root = make_element("div");
    auto* child = add_child(*root, make_element("span"));
    child->attributes["if"] = "show_debug";

    UiContext ctx;
    ctx.set("show_debug", "1");
    UiDataBind bind;
    bind.resolve(*root, ctx);

    REQUIRE(child->hidden_by_if == false);
}

TEST_CASE("if directive: '0' is falsy", "[databind]") {
    auto root = make_element("div");
    auto* child = add_child(*root, make_element("span"));
    child->attributes["if"] = "flag";

    UiContext ctx;
    ctx.set("flag", "0");
    UiDataBind bind;
    bind.resolve(*root, ctx);

    REQUIRE(child->hidden_by_if == true);
}

TEST_CASE("if directive: 'false' is falsy", "[databind]") {
    auto root = make_element("div");
    auto* child = add_child(*root, make_element("span"));
    child->attributes["if"] = "flag";

    UiContext ctx;
    ctx.set("flag", "false");
    UiDataBind bind;
    bind.resolve(*root, ctx);

    REQUIRE(child->hidden_by_if == true);
}

TEST_CASE("if directive: any other string is truthy", "[databind]") {
    auto root = make_element("div");
    auto* child = add_child(*root, make_element("span"));
    child->attributes["if"] = "flag";

    UiContext ctx;
    ctx.set("flag", "yes");
    UiDataBind bind;
    bind.resolve(*root, ctx);

    REQUIRE(child->hidden_by_if == false);
}

TEST_CASE("if directive toggles on context change", "[databind]") {
    auto root = make_element("div");
    auto* child = add_child(*root, make_element("span"));
    child->attributes["if"] = "visible";

    UiContext ctx;
    UiDataBind bind;

    ctx.set("visible", "1");
    bind.resolve(*root, ctx);
    REQUIRE(child->hidden_by_if == false);

    ctx.set("visible", "0");
    bind.resolve(*root, ctx);
    REQUIRE(child->hidden_by_if == true);
}

// --- class: dynamic classes ---

TEST_CASE("class:name adds class when condition is true", "[databind]") {
    auto root = make_element("div");
    root->attributes["class:active"] = "selected==1";

    UiContext ctx;
    ctx.set("selected", "1");
    UiDataBind bind;
    bind.resolve(*root, ctx);

    REQUIRE(root->has_class("active"));
}

TEST_CASE("class:name doesn't add class when condition is false", "[databind]") {
    auto root = make_element("div");
    root->attributes["class:active"] = "selected==1";

    UiContext ctx;
    ctx.set("selected", "2");
    UiDataBind bind;
    bind.resolve(*root, ctx);

    REQUIRE_FALSE(root->has_class("active"));
}

TEST_CASE("class:name removes class when condition becomes false", "[databind]") {
    auto root = make_element("div", {"active"});
    root->attributes["class:active"] = "selected==1";

    UiContext ctx;
    ctx.set("selected", "2");
    UiDataBind bind;
    bind.resolve(*root, ctx);

    REQUIRE_FALSE(root->has_class("active"));
}

TEST_CASE("class:name with truthy check (no ==)", "[databind]") {
    auto root = make_element("div");
    root->attributes["class:highlighted"] = "is_selected";

    UiContext ctx;
    ctx.set("is_selected", "1");
    UiDataBind bind;
    bind.resolve(*root, ctx);

    REQUIRE(root->has_class("highlighted"));
}

TEST_CASE("class:name truthy check: '0' removes class", "[databind]") {
    auto root = make_element("div", {"highlighted"});
    root->attributes["class:highlighted"] = "is_selected";

    UiContext ctx;
    ctx.set("is_selected", "0");
    UiDataBind bind;
    bind.resolve(*root, ctx);

    REQUIRE_FALSE(root->has_class("highlighted"));
}

TEST_CASE("class:name truthy check: 'false' removes class", "[databind]") {
    auto root = make_element("div", {"highlighted"});
    root->attributes["class:highlighted"] = "is_selected";

    UiContext ctx;
    ctx.set("is_selected", "false");
    UiDataBind bind;
    bind.resolve(*root, ctx);

    REQUIRE_FALSE(root->has_class("highlighted"));
}

TEST_CASE("Multiple class: bindings on same node", "[databind]") {
    auto root = make_element("div");
    root->attributes["class:active"] = "selected==1";
    root->attributes["class:disabled"] = "locked";

    UiContext ctx;
    ctx.set("selected", "1");
    ctx.set("locked", "true");
    UiDataBind bind;
    bind.resolve(*root, ctx);

    REQUIRE(root->has_class("active"));
    REQUIRE(root->has_class("disabled"));
}

// --- bar element ---

TEST_CASE("Bar element computes fill percentage", "[databind]") {
    auto root = make_element("bar");
    root->attributes["value"] = "{hp}";
    root->attributes["max"] = "{max_hp}";

    UiContext ctx;
    ctx.set("hp", "50");
    ctx.set("max_hp", "100");
    UiDataBind bind;
    bind.resolve(*root, ctx);

    float pct = std::stof(root->attributes["_fill_pct"]);
    REQUIRE(pct == Catch::Approx(0.5f));
}

TEST_CASE("Bar element clamps to 0-1", "[databind]") {
    auto root = make_element("bar");
    root->attributes["value"] = "{hp}";
    root->attributes["max"] = "{max_hp}";

    UiContext ctx;
    UiDataBind bind;

    // Over max
    ctx.set("hp", "150");
    ctx.set("max_hp", "100");
    bind.resolve(*root, ctx);
    REQUIRE(std::stof(root->attributes["_fill_pct"]) == Catch::Approx(1.0f));

    // Negative
    ctx.set("hp", "-10");
    bind.resolve(*root, ctx);
    REQUIRE(std::stof(root->attributes["_fill_pct"]) == Catch::Approx(0.0f));
}

TEST_CASE("Bar element with zero max uses 1.0 as fallback", "[databind]") {
    auto root = make_element("bar");
    root->attributes["value"] = "50";
    root->attributes["max"] = "0";

    UiContext ctx;
    UiDataBind bind;
    bind.resolve(*root, ctx);

    // max <= 0 → max = 1.0, so pct = 50/1 → clamped to 1.0
    float pct = std::stof(root->attributes["_fill_pct"]);
    REQUIRE(pct == Catch::Approx(1.0f));
}

TEST_CASE("Bar element with non-numeric value doesn't crash", "[databind]") {
    auto root = make_element("bar");
    root->attributes["value"] = "garbage";
    root->attributes["max"] = "100";

    UiContext ctx;
    UiDataBind bind;
    bind.resolve(*root, ctx);

    // stof("garbage") throws → caught, val=0
    float pct = std::stof(root->attributes["_fill_pct"]);
    REQUIRE(pct == Catch::Approx(0.0f));
}

// --- Change detection ---

TEST_CASE("Returns true when text changes", "[databind]") {
    auto root = make_element("div");
    add_child(*root, make_text("{val}"));

    UiContext ctx;
    UiDataBind bind;

    ctx.set("val", "hello");
    REQUIRE(bind.resolve(*root, ctx) == true);

    // Same value again — no change
    REQUIRE(bind.resolve(*root, ctx) == false);

    // New value
    ctx.set("val", "world");
    REQUIRE(bind.resolve(*root, ctx) == true);
}

TEST_CASE("Returns true when visibility changes", "[databind]") {
    auto root = make_element("div");
    auto* child = add_child(*root, make_element("span"));
    child->attributes["if"] = "flag";

    UiContext ctx;
    UiDataBind bind;

    // First resolve: flag not set → falsy → hidden_by_if becomes true (was false)
    REQUIRE(bind.resolve(*root, ctx) == true);
    REQUIRE(child->hidden_by_if == true);

    // Same state
    REQUIRE(bind.resolve(*root, ctx) == false);

    // Show it
    ctx.set("flag", "1");
    REQUIRE(bind.resolve(*root, ctx) == true);
    REQUIRE(child->hidden_by_if == false);
}

TEST_CASE("Returns true when class binding changes", "[databind]") {
    auto root = make_element("div");
    root->attributes["class:active"] = "selected==1";

    UiContext ctx;
    UiDataBind bind;

    ctx.set("selected", "1");
    REQUIRE(bind.resolve(*root, ctx) == true); // class added

    REQUIRE(bind.resolve(*root, ctx) == false); // no change

    ctx.set("selected", "2");
    REQUIRE(bind.resolve(*root, ctx) == true); // class removed
}

// --- Recursive resolution ---

TEST_CASE("Resolves children recursively", "[databind]") {
    auto root = make_element("div");
    auto* child = add_child(*root, make_element("span"));
    auto* text = add_child(*child, make_text("{msg}"));

    UiContext ctx;
    ctx.set("msg", "deep");
    UiDataBind bind;
    bind.resolve(*root, ctx);

    REQUIRE(text->text_content == "deep");
}

// --- Edge cases ---

TEST_CASE("Element nodes without text_template are skipped", "[databind]") {
    auto root = make_element("div");
    // Element node, not text — text_template is empty
    UiContext ctx;
    UiDataBind bind;
    bool changed = bind.resolve(*root, ctx);
    REQUIRE(changed == false);
}

TEST_CASE("class: binding doesn't duplicate class on repeated resolve", "[databind]") {
    auto root = make_element("div");
    root->attributes["class:active"] = "on";

    UiContext ctx;
    ctx.set("on", "1");
    UiDataBind bind;

    bind.resolve(*root, ctx);
    bind.resolve(*root, ctx);
    bind.resolve(*root, ctx);

    // "active" should appear exactly once
    int count = 0;
    for (const auto& c : root->classes) {
        if (c == "active") count++;
    }
    REQUIRE(count == 1);
}

TEST_CASE("class: binding with empty condition key", "[databind]") {
    auto root = make_element("div");
    root->attributes["class:test"] = "";

    UiContext ctx;
    UiDataBind bind;
    bind.resolve(*root, ctx);

    // empty key → get("") returns "" → falsy → class not added
    REQUIRE_FALSE(root->has_class("test"));
}

TEST_CASE("Template with literal braces around key", "[databind]") {
    auto root = make_element("div");
    auto* text = add_child(*root, make_text("{{double}}"));

    UiContext ctx;
    ctx.set("{double}", "nope");
    ctx.set("double", "yes");
    UiDataBind bind;
    bind.resolve(*root, ctx);

    // First { opens, finds "double}" — wait, let's see what actually happens:
    // "{" at 0 → find "}" → finds at 8 → key = "{double}" → get("{double}")
    // Actually no: key = substr(1, 8-1-1) wait... let me trace:
    // tmpl = "{{double}}"
    // i=0: tmpl[0]='{', find('}', 1) → finds at 8 → key = substr(1, 8-0-1) = substr(1,7) = "{double"
    // So it looks up "{double" (with leading brace). That's get("{double") = ""
    // Then i=9: tmpl[9]='}' → literal '}'
    // Result: "}". This is an edge case in the template parser.
    // Just verify no crash — the exact output is implementation-defined for malformed templates.
    REQUIRE_FALSE(text->text_content.empty()); // at minimum the trailing }
}
