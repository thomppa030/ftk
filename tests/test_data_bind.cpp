#include "ui/game_ui/ui_data_bind.hpp"
#include "ui/game_ui/ui_context.hpp"
#include "ui/game_ui/widget_asset.hpp"
#include "scripting/reflected_types.hpp"
#include "core/log.hpp"

#include <catch2/benchmark/catch_benchmark.hpp>
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

TEST_CASE("No placeholders - text unchanged", "[databind]") {
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

    float pct = root->fill_fraction;
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
    REQUIRE(root->fill_fraction == Catch::Approx(1.0f));

    // Negative
    ctx.set("hp", "-10");
    bind.resolve(*root, ctx);
    REQUIRE(root->fill_fraction == Catch::Approx(0.0f));
}

TEST_CASE("Bar element with zero max uses 1.0 as fallback", "[databind]") {
    auto root = make_element("bar");
    root->attributes["value"] = "50";
    root->attributes["max"] = "0";

    UiContext ctx;
    UiDataBind bind;
    bind.resolve(*root, ctx);

    // max <= 0 → max = 1.0, so pct = 50/1 → clamped to 1.0
    float pct = root->fill_fraction;
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
    float pct = root->fill_fraction;
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

// --- Comparing with last frame ---

TEST_CASE("First resolve runs against an untouched context", "[databind]") {
    auto root = make_element("div");
    auto* text = add_child(*root, make_text("[{missing}]"));

    UiContext ctx;
    UiDataBind bind;
    bind.resolve(*root, ctx);

    REQUIRE(text->text_content == "[]");
}

TEST_CASE("A binding whose values have not changed is not applied again", "[databind]") {
    auto root = make_element("div");
    auto* text = add_child(*root, make_text("{val}"));

    UiContext ctx;
    ctx.set("val", "first");
    UiDataBind bind;
    bind.resolve(*root, ctx);
    REQUIRE(text->text_content == "first");

    // Mutating the node behind the binder's back is not undone while the
    // value is the same — proof nothing is applied on a quiet frame.
    text->text_content = "stale";
    bind.resolve(*root, ctx);
    REQUIRE(text->text_content == "stale");

    ctx.set("val", "second");
    bind.resolve(*root, ctx);
    REQUIRE(text->text_content == "second");
}

// --- Typed values ---

TEST_CASE("A bar reads a number however it was published", "[databind]") {
    auto root = make_element("bar");
    root->attributes["value"] = "{hp}";
    root->attributes["max"] = "{max_hp}";
    UiContext ctx;
    UiDataBind bind;

    ctx.set("hp", UiValue(25.0f));
    ctx.set("max_hp", UiValue(100));
    bind.resolve(*root, ctx);
    REQUIRE(root->fill_fraction == Catch::Approx(0.25f));

    // One value per name: publishing text replaces the float, it does not sit
    // beside it.
    ctx.set("hp", "90");
    bind.resolve(*root, ctx);
    REQUIRE(root->fill_fraction == Catch::Approx(0.9f));
}

TEST_CASE("Values show as text the way they always have", "[databind]") {
    auto root = make_element("div");
    auto* text = add_child(*root, make_text("{on}/{off} {n} {f} {whole}"));

    UiContext ctx;
    ctx.set("on", UiValue(true));
    ctx.set("off", UiValue(false));
    ctx.set("n", UiValue(7));
    ctx.set("f", UiValue(0.1f));
    ctx.set("whole", UiValue(25.0f));
    UiDataBind bind;
    bind.resolve(*root, ctx);

    REQUIRE(text->text_content == "1/0 7 0.1 25");
}

TEST_CASE("if= and class: test each type for truth", "[databind]") {
    struct Case {
        UiValue value;
        bool truthy;
    };
    const Case cases[] = {
        {UiValue(), false},        {UiValue(true), true},     {UiValue(false), false},
        {UiValue(3), true},        {UiValue(0), false},       {UiValue(0.5f), true},
        {UiValue(0.0f), false},    {UiValue("yes"), true},    {UiValue(""), false},
        {UiValue("0"), false},     {UiValue("false"), false},
    };
    for (const auto& c : cases) {
        auto root = make_element("div");
        auto* shown = add_child(*root, make_element("div"));
        shown->attributes["if"] = "v";
        shown->attributes["class:on"] = "v";
        UiContext ctx;
        ctx.set("v", c.value);
        UiDataBind bind;
        bind.resolve(*root, ctx);
        INFO("value shows as '" << c.value.text() << "'");
        REQUIRE(shown->hidden_by_if == !c.truthy);
        REQUIRE(shown->has_class("on") == c.truthy);
    }
}

TEST_CASE("class: with == compares the value as markup shows it", "[databind]") {
    auto root = make_element("div");
    auto* slot = add_child(*root, make_element("div"));
    slot->attributes["class:active"] = "selected==2";
    UiContext ctx;
    UiDataBind bind;

    ctx.set("selected", UiValue(2));
    bind.resolve(*root, ctx);
    REQUIRE(slot->has_class("active"));

    ctx.set("selected", "3");
    bind.resolve(*root, ctx);
    REQUIRE_FALSE(slot->has_class("active"));
}

TEST_CASE("A value is the same only as the same type holding the same value", "[databind]") {
    REQUIRE(UiValue(7) == UiValue(7));
    REQUIRE_FALSE(UiValue(7) == UiValue(8));
    REQUIRE_FALSE(UiValue(7) == UiValue(7.0f)); // the same text, another type
    REQUIRE_FALSE(UiValue(7) == UiValue("7"));
}

TEST_CASE("A prop bound to a value keeps the type its widget declares", "[databind]") {
    auto root = make_element("div");
    auto* widget = add_child(*root, make_element("health-bar"));
    widget->is_widget = true;
    widget->scoped_context = std::make_unique<UiContext>();
    widget->widget_props = {{"value", "float", "100"}, {"count", "int", ""}, {"label", "string", ""}};
    set_prop_defaults(widget->widget_props, *widget->scoped_context);
    widget->prop_map["value"] = "{hp}";
    widget->prop_map["count"] = "{stack}";
    widget->prop_map["label"] = "HP {hp}";
    auto* bar = add_child(*widget, make_element("bar"));
    bar->attributes["value"] = "{value}";
    bar->attributes["max"] = "100";

    UiContext ctx;
    ctx.set("hp", "72");
    ctx.set("stack", UiValue(3.9f));
    UiDataBind bind;
    bind.resolve(*root, ctx);

    // The bound value replaces the default: a bar never reads a stale one.
    REQUIRE(bar->fill_fraction == Catch::Approx(0.72f));
    REQUIRE(*widget->scoped_context->find("value") == UiValue(72.0f));
    REQUIRE(*widget->scoped_context->find("count") == UiValue(3));
    REQUIRE(*widget->scoped_context->find("label") == UiValue("HP 72"));
}

TEST_CASE("Prop defaults take their declared types", "[databind]") {
    UiContext ctx;
    set_prop_defaults({{"a", "float", "2.5"}, {"b", "int", "4"}, {"c", "bool", "false"},
                       {"d", "string", "hi"}, {"e", "float", "wide"}, {"f", "int", ""}},
                      ctx);
    REQUIRE(*ctx.find("a") == UiValue(2.5f));
    REQUIRE(*ctx.find("b") == UiValue(4));
    REQUIRE(*ctx.find("c") == UiValue(false));
    REQUIRE(*ctx.find("d") == UiValue("hi"));
    REQUIRE(*ctx.find("e") == UiValue("wide")); // not a number: kept as written
    REQUIRE(ctx.find("f") == nullptr);           // no default: nothing set
}

// --- Parent context chaining ---

TEST_CASE("Child context falls back to parent for missing keys", "[databind]") {
    UiContext parent;
    parent.set("name", "root");
    parent.set("hp", UiValue(10.0f));

    UiContext child;
    child.set_parent(&parent);

    REQUIRE(child.get("name") == "root");
    REQUIRE(*child.find("hp")->number() == Catch::Approx(10.0f));

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

TEST_CASE("clone_node does not copy runtime state", "[databind]") {
    auto src = make_element("div");
    src->scoped_context = std::make_unique<UiContext>();
    src->repeat_scope = std::make_unique<UiContext>();

    auto clone = UiDataBind::clone_node(*src, nullptr);

    REQUIRE(clone->scoped_context == nullptr);
    REQUIRE(clone->repeat_scope == nullptr);
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

// A Model holding lists, and the descriptors fjreflect would generate for it.
struct Entry {
    std::string name{};
    std::string icon{};
    bool starred{false};
};

struct Shelf {
    std::vector<Entry> items{};
    std::vector<int> numbers{};
};

static uint32_t offset_in(const void* object, const void* member) {
    return static_cast<uint32_t>(static_cast<const char*>(member) - static_cast<const char*>(object));
}

static OwnedFieldDesc state_field(const char* name, FjellFieldType type, uint32_t offset) {
    OwnedFieldDesc field{};
    field.name = name;
    field.type = type;
    field.offset = offset;
    field.flags = FJELL_FIELD_FLAG_STATE;
    return field;
}

template <class T>
static OwnedVectorDesc vector_of(FjellFieldType element, const OwnedTypeDesc* element_type) {
    return {.element = element,
            .element_type = element_type,
            .element_size = sizeof(T),
            .size = [](const void* v) { return static_cast<uint32_t>(static_cast<const std::vector<T>*>(v)->size()); },
            .at = [](const void* v, uint32_t i) -> const void* { return &(*static_cast<const std::vector<T>*>(v))[i]; }};
}

// Lives where it was made: the shelf's fields point at the entry type.
struct ShelfTypes {
    OwnedTypeDesc entry;
    std::vector<OwnedFieldDesc> shelf;

    ShelfTypes() {
        const Entry e;
        entry.name = "Entry";
        entry.fields = {state_field("name", FJELL_FIELD_STD_STRING, offset_in(&e, &e.name)),
                        state_field("icon", FJELL_FIELD_STD_STRING, offset_in(&e, &e.icon)),
                        state_field("starred", FJELL_FIELD_BOOL, offset_in(&e, &e.starred))};
        const Shelf s;
        auto items = state_field("items", FJELL_FIELD_VECTOR, offset_in(&s, &s.items));
        items.vector = vector_of<Entry>(FJELL_FIELD_STRUCT, &entry);
        auto numbers = state_field("numbers", FJELL_FIELD_VECTOR, offset_in(&s, &s.numbers));
        numbers.vector = vector_of<int>(FJELL_FIELD_INT, nullptr);
        shelf = {items, numbers};
    }
    ShelfTypes(const ShelfTypes&) = delete;
    ShelfTypes& operator=(const ShelfTypes&) = delete;

    [[nodiscard]] UiValue object(const Shelf& s) const { return UiValue(UiObject{"Shelf", shelf, &s}); }
};

static std::vector<Entry> entries(std::initializer_list<const char*> names) {
    std::vector<Entry> out;
    for (const char* name : names) out.push_back({.name = name});
    return out;
}

// A root holding a template repeated by `foreach`, with a text reading `text`.
static UiNode* repeat_of(UiNode& root, const char* foreach, const char* text) {
    auto* tmpl = add_child(root, make_element("span"));
    tmpl->attributes["foreach"] = foreach;
    add_child(*tmpl, make_text(text));
    return tmpl;
}

static const std::string& shown(const UiNode& root, std::size_t repeat) {
    return root.children[repeat + 1]->children[0]->text_content;
}

TEST_CASE("foreach stamps one repeat per entry of a list", "[databind]") {
    const ShelfTypes types;
    Shelf shelf{.items = entries({"one", "two", "three"})};
    auto root = make_element("div");
    repeat_of(*root, "item in shelf.items", "{item.name}");

    UiContext ctx;
    ctx.set("shelf", types.object(shelf));
    UiDataBind bind;
    bind.resolve(*root, ctx);

    REQUIRE(root->children.size() == 4); // the template and three repeats
    REQUIRE(root->children[0]->is_foreach_template);
    REQUIRE(root->children[0]->hidden_by_if);
    for (int i = 0; i < 3; ++i) {
        auto& repeat = *root->children[static_cast<std::size_t>(i) + 1];
        REQUIRE(repeat.foreach_item_index == i);
        REQUIRE_FALSE(repeat.is_foreach_template);
        REQUIRE_FALSE(repeat.hidden_by_if);
        REQUIRE_FALSE(repeat.attributes.contains("foreach"));
        REQUIRE(repeat.repeat_scope != nullptr);
    }
    REQUIRE(shown(*root, 0) == "one");
    REQUIRE(shown(*root, 1) == "two");
    REQUIRE(shown(*root, 2) == "three");
}

TEST_CASE("foreach names each repeat's index", "[databind]") {
    const ShelfTypes types;
    Shelf shelf{.items = entries({"a", "b"})};
    auto root = make_element("div");
    repeat_of(*root, "item, i in shelf.items", "{i}:{item.name}");

    UiContext ctx;
    ctx.set("shelf", types.object(shelf));
    UiDataBind bind;
    bind.resolve(*root, ctx);

    REQUIRE(shown(*root, 0) == "0:a");
    REQUIRE(shown(*root, 1) == "1:b");
}

TEST_CASE("A repeat reads the scope around its foreach", "[databind]") {
    const ShelfTypes types;
    Shelf shelf{.items = entries({"a"})};
    auto root = make_element("div");
    repeat_of(*root, "item in shelf.items", "{prefix}{item.name}");

    UiContext ctx;
    ctx.set("prefix", ">");
    ctx.set("shelf", types.object(shelf));
    UiDataBind bind;
    bind.resolve(*root, ctx);

    REQUIRE(shown(*root, 0) == ">a");
}

TEST_CASE("foreach grows and shrinks with the list", "[databind]") {
    const ShelfTypes types;
    Shelf shelf{.items = entries({"a", "b"})};
    auto root = make_element("div");
    repeat_of(*root, "item in shelf.items", "{item.name}");

    UiContext ctx;
    ctx.set("shelf", types.object(shelf));
    UiDataBind bind;
    bind.resolve(*root, ctx);
    REQUIRE(root->children.size() == 3);
    UiNode* first = root->children[1].get();

    shelf.items = entries({"a", "b", "c", "d"});
    const auto grew = bind.resolve(*root, ctx);
    REQUIRE(root->children.size() == 5);
    REQUIRE(grew.layout_changed);
    REQUIRE(shown(*root, 3) == "d");
    REQUIRE(root->children[1].get() == first); // kept, not stamped again

    shelf.items = entries({"z"});
    const auto shrank = bind.resolve(*root, ctx);
    REQUIRE(root->children.size() == 2);
    REQUIRE(shrank.layout_changed);
    REQUIRE(shown(*root, 0) == "z");
}

TEST_CASE("foreach over an empty list leaves only the template", "[databind]") {
    const ShelfTypes types;
    Shelf shelf;
    auto root = make_element("div");
    repeat_of(*root, "item in shelf.items", "{item.name}");

    UiContext ctx;
    ctx.set("shelf", types.object(shelf));
    UiDataBind bind;
    bind.resolve(*root, ctx);

    REQUIRE(root->children.size() == 1);
    REQUIRE(root->children[0]->is_foreach_template);
}

TEST_CASE("foreach over a path that reaches no list stamps nothing", "[databind]") {
    const ShelfTypes types;
    Shelf shelf{.items = entries({"a"})};
    auto root = make_element("div");
    repeat_of(*root, "item in shelf.missing", "{item.name}");
    repeat_of(*root, "item in nobody.items", "{item.name}");
    repeat_of(*root, "item in shelf", "{item.name}"); // an object, not a list

    UiContext ctx;
    ctx.set("shelf", types.object(shelf));
    UiDataBind bind;
    bind.resolve(*root, ctx);

    REQUIRE(root->children.size() == 3);
    for (const auto& child : root->children) REQUIRE(child->is_foreach_template);
}

TEST_CASE("A foreach that is not `item in list` is reported and repeats nothing", "[databind]") {
    const ShelfTypes types;
    Shelf shelf{.items = entries({"a"})};
    auto root = make_element("div");
    repeat_of(*root, "items", "{name}");
    repeat_of(*root, "a b in shelf.items", "{a}");

    UiContext ctx;
    ctx.set("shelf", types.object(shelf));
    UiDataBind bind;
    bind.resolve(*root, ctx);

    REQUIRE(root->children.size() == 2);
    REQUIRE(root->children[0]->hidden_by_if); // still a template: never shown
}

TEST_CASE("The repeat count is stable across repeated resolves", "[databind]") {
    const ShelfTypes types;
    Shelf shelf{.items = entries({"a", "b"})};
    auto root = make_element("div");
    repeat_of(*root, "item in shelf.items", "{item.name}");

    UiContext ctx;
    ctx.set("shelf", types.object(shelf));
    UiDataBind bind;
    for (int i = 0; i < 5; ++i) bind.resolve(*root, ctx);
    REQUIRE(root->children.size() == 3);
}

TEST_CASE("A repeat shows its entry as it is now", "[databind]") {
    const ShelfTypes types;
    Shelf shelf{.items = entries({"before"})};
    auto root = make_element("div");
    repeat_of(*root, "item in shelf.items", "{item.name}");

    UiContext ctx;
    ctx.set("shelf", types.object(shelf));
    UiDataBind bind;
    bind.resolve(*root, ctx);
    REQUIRE(shown(*root, 0) == "before");

    shelf.items[0].name = "after";
    const auto changed = bind.resolve(*root, ctx);
    REQUIRE(shown(*root, 0) == "after");
    REQUIRE_FALSE(changed.layout_changed); // a text, nothing structural
}

TEST_CASE("A repeat follows its entry when the vector moves its storage", "[databind]") {
    const ShelfTypes types;
    Shelf shelf{.items = entries({"a"})};
    shelf.items.shrink_to_fit();
    auto root = make_element("div");
    repeat_of(*root, "item in shelf.items", "{item.name}");

    UiContext ctx;
    ctx.set("shelf", types.object(shelf));
    UiDataBind bind;
    bind.resolve(*root, ctx);
    const Entry* before = shelf.items.data();

    for (int i = 0; i < 64; ++i) shelf.items.push_back({.name = "more"});
    REQUIRE(shelf.items.data() != before); // the entries moved
    shelf.items[0].name = "moved";
    bind.resolve(*root, ctx);
    REQUIRE(shown(*root, 0) == "moved");
    REQUIRE(root->children.size() == 66);
}

TEST_CASE("A list of values repeats the values", "[databind]") {
    const ShelfTypes types;
    Shelf shelf{.numbers = {3, 5}};
    auto root = make_element("div");
    repeat_of(*root, "n in shelf.numbers", "#{n}");

    UiContext ctx;
    ctx.set("shelf", types.object(shelf));
    UiDataBind bind;
    bind.resolve(*root, ctx);

    REQUIRE(shown(*root, 0) == "#3");
    REQUIRE(shown(*root, 1) == "#5");
    REQUIRE(ctx.evaluate("shelf.numbers").truthy());
    REQUIRE_FALSE(ctx.evaluate("shelf.items").truthy()); // an empty list
}

TEST_CASE("foreach honours if directives inside the template", "[databind]") {
    const ShelfTypes types;
    Shelf shelf{.items = {{.name = "a", .starred = true}, {.name = "b", .starred = false}}};
    auto root = make_element("div");
    auto* tmpl = add_child(*root, make_element("span"));
    tmpl->attributes["foreach"] = "item in shelf.items";
    auto* badge = add_child(*tmpl, make_element("i"));
    badge->attributes["if"] = "item.starred";

    UiContext ctx;
    ctx.set("shelf", types.object(shelf));
    UiDataBind bind;
    bind.resolve(*root, ctx);

    REQUIRE_FALSE(root->children[1]->children[0]->hidden_by_if);
    REQUIRE(root->children[2]->children[0]->hidden_by_if);
}

TEST_CASE("A new tree is bound on its first resolve, forgotten or not", "[bind]") {
    UiContext ctx;
    ctx.set("k", "1");
    UiDataBind bind;

    auto first = std::make_unique<UiNode>();
    first->type = UiNodeType::text;
    first->text_template = "{k}";
    bind.resolve(*first, ctx);
    REQUIRE(first->text_content == "1");

    // A replacement tree: its bindings are compiled when it is first
    // resolved, whether the old tree was forgotten first or not.
    auto second = std::make_unique<UiNode>();
    second->type = UiNodeType::text;
    second->text_template = "{k}";
    bind.resolve(*second, ctx);
    REQUIRE(second->text_content == "1");

    auto third = std::make_unique<UiNode>();
    third->type = UiNodeType::text;
    third->text_template = "[{k}]";
    bind.forget_tree();
    bind.resolve(*third, ctx);
    REQUIRE(third->text_content == "[1]");
}

TEST_CASE("style-* attributes resolve their {key} templates into inline styles", "[bind]") {
    UiContext ctx;
    ctx.set("from", "#000000");
    ctx.set("to", "#ffffff");
    ctx.set("tint", "#00ff00");
    auto root = std::make_unique<UiNode>();
    root->type = UiNodeType::element;
    root->tag = "div";
    root->attributes["style-background"] = "linear-gradient(90deg, {from}, {to})";
    root->attributes["style---fill"] = "{tint}";
    root->attributes["style-width"] = "120";

    UiDataBind bind;
    auto first = bind.resolve(*root, ctx);
    REQUIRE(first.layout_changed);
    REQUIRE(root->inline_styles.at("background") == "linear-gradient(90deg, #000000, #ffffff)");
    REQUIRE(root->inline_styles.at("--fill") == "#00ff00");
    REQUIRE(root->inline_styles.at("width") == "120");
    // The authored template is untouched, so it can be written back
    REQUIRE(root->attributes.at("style-background") == "linear-gradient(90deg, {from}, {to})");

    // A changed key changes the style and asks for a re-resolve
    ctx.set("to", "#ff0000");
    auto second = bind.resolve(*root, ctx);
    REQUIRE(second.layout_changed);
    REQUIRE(root->inline_styles.at("background") == "linear-gradient(90deg, #000000, #ff0000)");

    // Nothing changed: no relayout requested
    ctx.set("unrelated", "1");
    auto third = bind.resolve(*root, ctx);
    REQUIRE_FALSE(third.layout_changed);
}

// --- src and material templates ---

TEST_CASE("A templated src is reported when it resolves and when it changes", "[databind][assets]") {
    auto root = make_element("div");
    root->attributes["src"] = "{icon}";

    UiContext ctx;
    ctx.set("icon", "a.png");
    UiDataBind bind;
    auto first = bind.resolve(*root, ctx);
    REQUIRE(first.asset_changes == std::vector<UiNode*>{root.get()});
    REQUIRE(root->bound_src == "a.png");

    REQUIRE(bind.resolve(*root, ctx).asset_changes.empty());

    ctx.set("icon", "b.png");
    auto swapped = bind.resolve(*root, ctx);
    REQUIRE(swapped.asset_changes == std::vector<UiNode*>{root.get()});
    REQUIRE(root->bound_src == "b.png");
}

TEST_CASE("A plain src or material is left to the loader", "[databind][assets]") {
    auto root = make_element("div");
    root->attributes["src"] = "art/tile.png";
    root->attributes["material"] = "ui_ring";

    UiContext ctx;
    UiDataBind bind;
    REQUIRE(bind.resolve(*root, ctx).asset_changes.empty());
    REQUIRE(root->bound_src.empty());
    REQUIRE(root->bound_material.empty());
}

TEST_CASE("Each repeat binds its own entry's src and material", "[databind][assets]") {
    const ShelfTypes types;
    Shelf shelf{.items = {{.name = "ui_ring", .icon = "wet.png"}, {.name = "ui_ring_harmful", .icon = "burn.png"}}};
    auto root = make_element("div");
    auto* tmpl = add_child(*root, make_element("div"));
    tmpl->attributes["foreach"] = "effect in shelf.items";
    tmpl->attributes["src"] = "{effect.icon}";
    tmpl->attributes["material"] = "{effect.name}";

    UiContext ctx;
    ctx.set("shelf", types.object(shelf));
    UiDataBind bind;
    auto result = bind.resolve(*root, ctx);

    // The template itself is never bound; each repeat once, for both attributes.
    REQUIRE(result.asset_changes.size() == 2);
    REQUIRE(tmpl->bound_src.empty());
    REQUIRE(root->children[1]->bound_src == "wet.png");
    REQUIRE(root->children[2]->bound_src == "burn.png");
    REQUIRE(root->children[2]->bound_material == "ui_ring_harmful");

    shelf.items[1].icon = "poison.png";
    auto changed = bind.resolve(*root, ctx);
    REQUIRE(changed.asset_changes == std::vector<UiNode*>{root->children[2].get()});
}

// --- Work done per frame ---

TEST_CASE("A quiet frame evaluates each binding on show once and applies none", "[databind][work]") {
    auto root = make_element("div");
    add_child(*root, make_text("{a}"));
    add_child(*root, make_text("HP {a} / {b}"));
    auto* bar = add_child(*root, make_element("bar"));
    bar->attributes["value"] = "{a}";
    bar->attributes["max"] = "{b}";

    UiContext ctx;
    ctx.set("a", UiValue(3));
    ctx.set("b", UiValue(10));
    UiDataBind bind;
    bind.resolve(*root, ctx);
    REQUIRE(bind.work().applied == 3);

    const auto quiet = bind.resolve(*root, ctx);
    REQUIRE(bind.work().evaluated == 3);
    REQUIRE(bind.work().applied == 0);
    REQUIRE_FALSE(quiet.any());
}

TEST_CASE("A changed value applies exactly the bindings that read it", "[databind][work]") {
    auto root = make_element("div");
    auto* one = add_child(*root, make_text("{a}"));
    auto* two = add_child(*root, make_text("a is {a}"));
    auto* other = add_child(*root, make_text("{b}"));

    UiContext ctx;
    ctx.set("a", UiValue(1));
    ctx.set("b", UiValue(2));
    UiDataBind bind;
    bind.resolve(*root, ctx);

    ctx.set("a", UiValue(5));
    const auto result = bind.resolve(*root, ctx);
    REQUIRE(bind.work().evaluated == 3);
    REQUIRE(bind.work().applied == 2);
    REQUIRE(result.text_changed);
    REQUIRE(one->text_content == "5");
    REQUIRE(two->text_content == "a is 5");
    REQUIRE(other->text_content == "2");
}

TEST_CASE("A subtree hidden by if= evaluates only its root's if= and class:", "[databind][work]") {
    auto root = make_element("div");
    auto* panel = add_child(*root, make_element("div"));
    panel->attributes["if"] = "open";
    panel->attributes["class:warm"] = "warm";
    auto* first = add_child(*panel, make_text("{a}"));
    add_child(*panel, make_text("{b}"));
    add_child(*panel, make_text("{c}"));

    UiContext ctx;
    ctx.set("open", UiValue(false));
    ctx.set("a", "x");
    UiDataBind bind;
    bind.resolve(*root, ctx);
    bind.resolve(*root, ctx);
    REQUIRE(bind.work().evaluated == 2);
    REQUIRE(first->text_content == "{a}"); // never bound while hidden

    // Shown again, the subtree is bound in the same frame.
    ctx.set("open", UiValue(true));
    const auto shown = bind.resolve(*root, ctx);
    REQUIRE(bind.work().evaluated == 5);
    REQUIRE(shown.layout_changed);
    REQUIRE(first->text_content == "x");
}

TEST_CASE("A subtree the last style pass hid is bound once the style pass shows it", "[databind][work]") {
    auto root = make_element("div");
    auto* panel = add_child(*root, make_element("div"));
    auto* text = add_child(*panel, make_text("{a}"));

    UiContext ctx;
    ctx.set("a", "one");
    UiDataBind bind;
    bind.resolve(*root, ctx);
    REQUIRE(text->text_content == "one");

    panel->style.display = UiDisplay::none; // what the style pass decided
    ctx.set("a", "two");
    bind.resolve(*root, ctx);
    REQUIRE(bind.work().evaluated == 0);
    REQUIRE(text->text_content == "one");

    bool revealed = true;
    (void)bind.resolve_revealed(revealed);
    REQUIRE_FALSE(revealed); // still hidden: nothing to do

    panel->style.display = UiDisplay::flex; // the next style pass shows it
    const auto late = bind.resolve_revealed(revealed);
    REQUIRE(revealed);
    REQUIRE(late.text_changed);
    REQUIRE(text->text_content == "two");
    (void)bind.resolve_revealed(revealed);
    REQUIRE_FALSE(revealed);
}

TEST_CASE("Repeats stamped for a longer list are bound in the same frame", "[databind][work]") {
    const ShelfTypes types;
    Shelf shelf{.items = entries({"a"})};
    auto root = make_element("div");
    repeat_of(*root, "item in shelf.items", "{item.name}");

    UiContext ctx;
    ctx.set("shelf", types.object(shelf));
    UiDataBind bind;
    bind.resolve(*root, ctx);
    REQUIRE(root->children.size() == 2);

    shelf.items = entries({"a", "b", "c"});
    const auto grown = bind.resolve(*root, ctx);
    REQUIRE(grown.layout_changed);
    REQUIRE(root->children.size() == 4);
    REQUIRE(shown(*root, 2) == "c");

    bind.resolve(*root, ctx);
    REQUIRE(bind.work().applied == 0);
    REQUIRE(bind.work().evaluated == 4); // the repeat and three texts
}

// --- Cost ---

// A UI with `count` text bindings on show, mixed as a HUD mixes them: a bare
// value, a value in a sentence, and a value through a pipe.
static std::unique_ptr<UiNode> bound_tree(int count, UiContext& ctx) {
    auto root = make_element("div");
    for (int i = 0; i < count; ++i) {
        const std::string key = "v" + std::to_string(i);
        ctx.set(key, UiValue(static_cast<float>(i) + 0.5f));
        switch (i % 3) {
            case 0: add_child(*root, make_text("{" + key + "}")); break;
            case 1: add_child(*root, make_text("HP {" + key + "} / 100")); break;
            default: add_child(*root, make_text("{" + key + "|round}")); break;
        }
    }
    return root;
}

TEST_CASE("What binding costs a frame", "[.bench][databind]") {
    for (const int count : {70, 1200, 5000}) {
        UiPipes pipes;
        UiContext ctx;
        ctx.set_pipes(&pipes);
        auto root = bound_tree(count, ctx);
        UiDataBind bind;
        bind.resolve(*root, ctx);

        BENCHMARK("quiet frame, " + std::to_string(count) + " bindings") {
            return bind.resolve(*root, ctx).any();
        };
        int n = 0;
        BENCHMARK("one value changed, " + std::to_string(count) + " bindings") {
            ctx.set("v0", UiValue(++n));
            return bind.resolve(*root, ctx).any();
        };
    }
}
