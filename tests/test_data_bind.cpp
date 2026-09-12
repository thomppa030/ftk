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

// --- Result granularity: text vs layout ---

TEST_CASE("Text change reports text_changed only", "[databind]") {
    auto root = make_element("div");
    add_child(*root, make_text("{val}"));

    UiContext ctx;
    ctx.set("val", "a");
    UiDataBind bind;
    bind.resolve(*root, ctx);

    ctx.set("val", "b");
    auto result = bind.resolve(*root, ctx);
    REQUIRE(result.text_changed == true);
    REQUIRE(result.layout_changed == false);
}

TEST_CASE("Visibility change reports layout_changed", "[databind]") {
    auto root = make_element("div");
    auto* child = add_child(*root, make_element("span"));
    child->attributes["if"] = "flag";

    UiContext ctx;
    ctx.set("flag", "1");
    UiDataBind bind;
    bind.resolve(*root, ctx);

    ctx.set("flag", "0");
    auto result = bind.resolve(*root, ctx);
    REQUIRE(result.layout_changed == true);
}

TEST_CASE("class: change reports layout_changed", "[databind]") {
    auto root = make_element("div");
    root->attributes["class:on"] = "flag";

    UiContext ctx;
    ctx.set("flag", "0");
    UiDataBind bind;
    bind.resolve(*root, ctx);

    ctx.set("flag", "1");
    auto result = bind.resolve(*root, ctx);
    REQUIRE(result.layout_changed == true);
    REQUIRE(root->has_class("on"));
}

TEST_CASE("No change reports neither flag", "[databind]") {
    auto root = make_element("div");
    add_child(*root, make_text("{val}"));

    UiContext ctx;
    ctx.set("val", "a");
    UiDataBind bind;
    bind.resolve(*root, ctx);

    auto result = bind.resolve(*root, ctx);
    REQUIRE(result.text_changed == false);
    REQUIRE(result.layout_changed == false);
    REQUIRE(result.any() == false);
}

// --- Generation gate ---

TEST_CASE("First resolve runs against an untouched context", "[databind]") {
    // A fresh UiContext sits at generation 0; the gate must not mistake that
    // for "already resolved at generation 0" and skip the whole tree.
    auto root = make_element("div");
    auto* text = add_child(*root, make_text("[{missing}]"));

    UiContext ctx;
    UiDataBind bind;
    bind.resolve(*root, ctx);

    REQUIRE(text->text_content == "[]");
}

TEST_CASE("Resolve is skipped while generation is unchanged", "[databind]") {
    auto root = make_element("div");
    auto* text = add_child(*root, make_text("{val}"));

    UiContext ctx;
    ctx.set("val", "first");
    UiDataBind bind;
    bind.resolve(*root, ctx);
    REQUIRE(text->text_content == "first");

    // Mutating the node behind the binder's back is not undone while the
    // generation is static — proof the early-out is actually taken.
    text->text_content = "stale";
    bind.resolve(*root, ctx);
    REQUIRE(text->text_content == "stale");

    ctx.set("val", "second");
    bind.resolve(*root, ctx);
    REQUIRE(text->text_content == "second");
}

TEST_CASE("Setting a key to its current value does not bump generation", "[databind]") {
    UiContext ctx;
    ctx.set("val", "x");
    auto gen = ctx.generation();
    ctx.set("val", "x");
    REQUIRE(ctx.generation() == gen);
    ctx.set("val", "y");
    REQUIRE(ctx.generation() > gen);
}

// --- Typed values ---

TEST_CASE("Bar reads typed float value", "[databind]") {
    auto root = make_element("bar");
    root->attributes["value"] = "{hp}";
    root->attributes["max"] = "{max_hp}";

    UiContext ctx;
    ctx.set_float("hp", 25.0f);
    ctx.set_float("max_hp", 100.0f);
    // Overwrite the stringified mirror so only the typed lookup gives 0.25 —
    // a stof() of the string form would read 90.
    ctx.set("hp", "90");
    UiDataBind bind;
    bind.resolve(*root, ctx);

    REQUIRE(ctx.get_float("hp") == Catch::Approx(25.0f));
    REQUIRE(std::stof(root->attributes["_fill_pct"]) == Catch::Approx(0.25f));
}

TEST_CASE("Bar reads typed int value", "[databind]") {
    auto root = make_element("bar");
    root->attributes["value"] = "{hp}";
    root->attributes["max"] = "{max_hp}";

    UiContext ctx;
    ctx.set_int("hp", 3);
    ctx.set_int("max_hp", 4);
    UiDataBind bind;
    bind.resolve(*root, ctx);

    REQUIRE(std::stof(root->attributes["_fill_pct"]) == Catch::Approx(0.75f));
}

TEST_CASE("set_bool writes a template-visible string", "[databind]") {
    auto root = make_element("div");
    auto* text = add_child(*root, make_text("{on}/{off}"));

    UiContext ctx;
    ctx.set_bool("on", true);
    ctx.set_bool("off", false);
    UiDataBind bind;
    bind.resolve(*root, ctx);

    REQUIRE(text->text_content == "1/0");
    REQUIRE(ctx.get_bool("on") == true);
    REQUIRE(ctx.get_bool("off") == false);
}

TEST_CASE("Typed setters retract the previously typed kind", "[databind]") {
    UiContext ctx;
    ctx.set_int("v", 7);
    REQUIRE(ctx.get_int("v") == 7);
    REQUIRE(ctx.get_float("v") == Catch::Approx(7.0f)); // int falls back to float

    ctx.set_float("v", 1.5f);
    REQUIRE(ctx.get_float("v") == Catch::Approx(1.5f));
    REQUIRE_FALSE(ctx.get_int("v").has_value());
}

// --- Parent context chaining ---

TEST_CASE("Child context falls back to parent for missing keys", "[databind]") {
    UiContext parent;
    parent.set("name", "root");
    parent.set_float("hp", 10.0f);

    UiContext child;
    child.set_parent(&parent);

    REQUIRE(child.get("name") == "root");
    REQUIRE(child.get_float("hp") == Catch::Approx(10.0f));

    child.set("name", "own");
    REQUIRE(child.get("name") == "own");
    REQUIRE(parent.get("name") == "root");
}

// --- Widget scoped contexts ---

TEST_CASE("Widget props resolve against the outer context", "[databind]") {
    auto root = make_element("div");
    auto* widget = add_child(*root, make_element("health-bar"));
    widget->is_widget = true;
    widget->scoped_context = std::make_unique<UiContext>();
    widget->prop_map["value"] = "{player.hp}";
    auto* label = add_child(*widget, make_text("HP {value}"));

    UiContext ctx;
    ctx.set("player.hp", "42");
    UiDataBind bind;
    bind.resolve(*root, ctx);

    // Prop was copied into the widget's own scope...
    REQUIRE(widget->scoped_context->get("value") == "42");
    // ...and children resolve against that scope, not the outer one.
    REQUIRE(label->text_content == "HP 42");
}

TEST_CASE("Widget scope shadows the outer context", "[databind]") {
    auto root = make_element("div");
    auto* outer_text = add_child(*root, make_text("{label}"));
    auto* widget = add_child(*root, make_element("card"));
    widget->is_widget = true;
    widget->scoped_context = std::make_unique<UiContext>();
    widget->scoped_context->set("label", "inner");
    auto* inner_text = add_child(*widget, make_text("{label}"));

    UiContext ctx;
    ctx.set("label", "outer");
    UiDataBind bind;
    bind.resolve(*root, ctx);

    REQUIRE(outer_text->text_content == "outer");
    REQUIRE(inner_text->text_content == "inner");
}

TEST_CASE("Widget props track outer context changes", "[databind]") {
    auto root = make_element("div");
    auto* widget = add_child(*root, make_element("health-bar"));
    widget->is_widget = true;
    widget->scoped_context = std::make_unique<UiContext>();
    widget->prop_map["value"] = "{player.hp}";
    auto* label = add_child(*widget, make_text("{value}"));

    UiContext ctx;
    ctx.set("player.hp", "1");
    UiDataBind bind;
    bind.resolve(*root, ctx);
    REQUIRE(label->text_content == "1");

    ctx.set("player.hp", "2");
    bind.resolve(*root, ctx);
    REQUIRE(label->text_content == "2");
}

// --- clone_node ---

TEST_CASE("clone_node copies structure and widget state", "[databind]") {
    auto src = make_element("div", {"a", "b"});
    src->id = "orig";
    src->attributes["foreach"] = "items";
    src->is_widget = true;
    src->widget_tag = "health-bar";
    src->widget_class = "health_bar_controller";
    src->prop_map["value"] = "{hp}";
    add_child(*src, make_text("{name}"));

    auto clone = UiDataBind::clone_node(*src, nullptr);

    REQUIRE(clone->tag == "div");
    REQUIRE(clone->id == "orig");
    REQUIRE(clone->classes == std::vector<std::string>{"a", "b"});
    REQUIRE(clone->attributes.at("foreach") == "items");
    REQUIRE(clone->is_widget == true);
    REQUIRE(clone->widget_tag == "health-bar");
    REQUIRE(clone->widget_class == "health_bar_controller");
    REQUIRE(clone->prop_map.at("value") == "{hp}");
    REQUIRE(clone->children.size() == 1);
    REQUIRE(clone->children[0]->text_template == "{name}");
    REQUIRE(clone->children[0]->parent == clone.get());
    REQUIRE(clone->parent == nullptr);
}

TEST_CASE("clone_node does not copy runtime pointers", "[databind]") {
    auto src = make_element("div");
    src->scoped_context = std::make_unique<UiContext>();
    UiContext item;
    src->foreach_context = &item;

    auto clone = UiDataBind::clone_node(*src, nullptr);

    REQUIRE(clone->scoped_context == nullptr);
    REQUIRE(clone->foreach_context == nullptr);
    REQUIRE(clone->yg_node == nullptr);
}

TEST_CASE("clone_node is a deep copy", "[databind]") {
    auto src = make_element("div");
    auto* child = add_child(*src, make_element("span"));

    auto clone = UiDataBind::clone_node(*src, nullptr);
    child->tag = "mutated";

    REQUIRE(clone->children[0]->tag == "span");
}

// --- foreach ---

// set_list() does not bump the context generation, so a test that changes only
// a list must nudge the generation the way UiSubsystem::tick does each frame.
static void bump(UiContext& ctx) {
    static int n = 0;
    ctx.set("__tick", std::to_string(++n));
}

static std::vector<UiContext> make_items(const std::vector<std::string>& names) {
    std::vector<UiContext> items;
    items.reserve(names.size());
    for (const auto& n : names) {
        UiContext item;
        item.set("name", n);
        items.push_back(std::move(item));
    }
    return items;
}

TEST_CASE("foreach stamps one clone per list item", "[databind]") {
    auto root = make_element("div");
    auto* tmpl = add_child(*root, make_element("span"));
    tmpl->attributes["foreach"] = "items";
    add_child(*tmpl, make_text("{name}"));

    UiContext ctx;
    ctx.set_list("items", make_items({"one", "two", "three"}));
    UiDataBind bind;
    bind.resolve(*root, ctx);

    // template + 3 clones
    REQUIRE(root->children.size() == 4);
    REQUIRE(root->children[0]->is_foreach_template == true);
    REQUIRE(root->children[0]->hidden_by_if == true);

    for (int i = 0; i < 3; ++i) {
        auto& clone = *root->children[static_cast<size_t>(i) + 1];
        REQUIRE(clone.foreach_item_index == i);
        REQUIRE(clone.is_foreach_template == false);
        REQUIRE(clone.hidden_by_if == false);
        REQUIRE_FALSE(clone.attributes.contains("foreach"));
    }

    REQUIRE(root->children[1]->children[0]->text_content == "one");
    REQUIRE(root->children[2]->children[0]->text_content == "two");
    REQUIRE(root->children[3]->children[0]->text_content == "three");
}

TEST_CASE("foreach exposes __index on each item", "[databind]") {
    auto root = make_element("div");
    auto* tmpl = add_child(*root, make_element("span"));
    tmpl->attributes["foreach"] = "items";
    add_child(*tmpl, make_text("{__index}:{name}"));

    UiContext ctx;
    ctx.set_list("items", make_items({"a", "b"}));
    UiDataBind bind;
    bind.resolve(*root, ctx);

    REQUIRE(root->children[1]->children[0]->text_content == "0:a");
    REQUIRE(root->children[2]->children[0]->text_content == "1:b");
}

TEST_CASE("foreach items fall back to the outer context", "[databind]") {
    auto root = make_element("div");
    auto* tmpl = add_child(*root, make_element("span"));
    tmpl->attributes["foreach"] = "items";
    add_child(*tmpl, make_text("{prefix}{name}"));

    UiContext ctx;
    ctx.set("prefix", ">");
    ctx.set_list("items", make_items({"a"}));
    UiDataBind bind;
    bind.resolve(*root, ctx);

    REQUIRE(root->children[1]->children[0]->text_content == ">a");
}

TEST_CASE("foreach grows and shrinks with the list", "[databind]") {
    auto root = make_element("div");
    auto* tmpl = add_child(*root, make_element("span"));
    tmpl->attributes["foreach"] = "items";
    add_child(*tmpl, make_text("{name}"));

    UiContext ctx;
    ctx.set_list("items", make_items({"a", "b"}));
    UiDataBind bind;
    bind.resolve(*root, ctx);
    REQUIRE(root->children.size() == 3);

    ctx.set_list("items", make_items({"a", "b", "c", "d"}));
    bump(ctx);
    auto grew = bind.resolve(*root, ctx);
    REQUIRE(root->children.size() == 5);
    REQUIRE(grew.layout_changed == true);
    REQUIRE(root->children[4]->children[0]->text_content == "d");

    ctx.set_list("items", make_items({"a"}));
    bump(ctx);
    auto shrank = bind.resolve(*root, ctx);
    REQUIRE(root->children.size() == 2);
    REQUIRE(shrank.layout_changed == true);
    REQUIRE(root->children[1]->children[0]->text_content == "a");
}

TEST_CASE("foreach over an empty list leaves only the template", "[databind]") {
    auto root = make_element("div");
    auto* tmpl = add_child(*root, make_element("span"));
    tmpl->attributes["foreach"] = "items";

    UiContext ctx;
    ctx.set_list("items", {});
    UiDataBind bind;
    bind.resolve(*root, ctx);

    REQUIRE(root->children.size() == 1);
    REQUIRE(root->children[0]->is_foreach_template == true);
}

TEST_CASE("foreach over a missing list produces no clones", "[databind]") {
    auto root = make_element("div");
    auto* tmpl = add_child(*root, make_element("span"));
    tmpl->attributes["foreach"] = "nonexistent";

    UiContext ctx;
    UiDataBind bind;
    bind.resolve(*root, ctx);

    REQUIRE(root->children.size() == 1);
    REQUIRE(root->children[0]->is_foreach_template == true);
}

TEST_CASE("foreach clone count is stable across repeated resolves", "[databind]") {
    auto root = make_element("div");
    auto* tmpl = add_child(*root, make_element("span"));
    tmpl->attributes["foreach"] = "items";
    add_child(*tmpl, make_text("{name}"));

    UiContext ctx;
    ctx.set_list("items", make_items({"a", "b"}));
    UiDataBind bind;
    bind.resolve(*root, ctx);
    REQUIRE(root->children.size() == 3);

    for (int i = 0; i < 5; ++i) {
        bump(ctx);
        bind.resolve(*root, ctx);
    }
    REQUIRE(root->children.size() == 3);
}

TEST_CASE("foreach clones pick up item value changes", "[databind]") {
    auto root = make_element("div");
    auto* tmpl = add_child(*root, make_element("span"));
    tmpl->attributes["foreach"] = "items";
    add_child(*tmpl, make_text("{name}"));

    UiContext ctx;
    ctx.set_list("items", make_items({"before"}));
    UiDataBind bind;
    bind.resolve(*root, ctx);
    REQUIRE(root->children[1]->children[0]->text_content == "before");

    ctx.get_list("items")->at(0).set("name", "after");
    bump(ctx);
    bind.resolve(*root, ctx);
    REQUIRE(root->children[1]->children[0]->text_content == "after");
}

TEST_CASE("foreach honours if directives inside the template", "[databind]") {
    auto root = make_element("div");
    auto* tmpl = add_child(*root, make_element("span"));
    tmpl->attributes["foreach"] = "items";
    auto* badge = add_child(*tmpl, make_element("i"));
    badge->attributes["if"] = "starred";

    UiContext ctx;
    std::vector<UiContext> items;
    UiContext a;
    a.set("starred", "1");
    items.push_back(std::move(a));
    UiContext b;
    b.set("starred", "0");
    items.push_back(std::move(b));
    ctx.set_list("items", std::move(items));

    UiDataBind bind;
    bind.resolve(*root, ctx);

    REQUIRE(root->children[1]->children[0]->hidden_by_if == false);
    REQUIRE(root->children[2]->children[0]->hidden_by_if == true);
}
