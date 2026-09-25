#include "scripting/reflected_types.hpp"
#include "ui/game_ui/ui_context.hpp"
#include "ui/game_ui/ui_data_bind.hpp"
#include "ui/game_ui/ui_pipes.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

using namespace fjell;

namespace {

// A root scope formatting with `pipes`, as a GameUi's is.
std::string show(const std::string& tmpl, UiContext& ctx) { return UiDataBind::resolve_template(tmpl, ctx); }

} // namespace

TEST_CASE("A binding splits into its value and its pipes", "[ui_pipes]") {
    const auto expr = parse_expression(" copper | coins:' gold: each' | fixed:2 ");
    REQUIRE(expr.path == "copper");
    REQUIRE(expr.pipes.size() == 2);
    REQUIRE(expr.pipes[0].name == "coins");
    REQUIRE(expr.pipes[0].args == std::vector<std::string>{" gold: each"});
    REQUIRE(expr.pipes[1].name == "fixed");
    REQUIRE(expr.pipes[1].args == std::vector<std::string>{"2"});
    REQUIRE(parse_expression("hp").pipes.empty());
}

TEST_CASE("The engine's pipes format numbers", "[ui_pipes]") {
    UiPipes pipes;
    UiContext ctx;
    ctx.set_pipes(&pipes);
    ctx.set("stamina", UiValue(117.32f));
    ctx.set("ratio", UiValue(0.1234f));
    ctx.set("count", UiValue(3));
    ctx.set("name", "Axe");

    REQUIRE(show("{stamina|round}", ctx) == "117");
    REQUIRE(show("{stamina|fixed:1}", ctx) == "117.3");
    REQUIRE(show("{ratio|percent}", ctx) == "12%");
    REQUIRE(show("{ratio|percent:1}", ctx) == "12.3%");
    REQUIRE(show("{count|times}", ctx) == "×3");
    REQUIRE(show("{name|round}", ctx) == "Axe"); // not a number: left as it is
    REQUIRE(show("{stamina|round|times}", ctx) == "×117"); // left to right
}

TEST_CASE("round takes halves away from zero", "[ui_pipes]") {
    UiPipes pipes;
    UiContext ctx;
    ctx.set_pipes(&pipes);
    const std::pair<float, const char*> cases[] = {
        {116.5f, "117"}, {2.5f, "3"}, {3.5f, "4"}, {-2.5f, "-3"}, {-0.4f, "0"}, {0.49f, "0"},
    };
    for (const auto& [value, text] : cases) {
        ctx.set("v", UiValue(value));
        INFO(value);
        REQUIRE(show("{v|round}", ctx) == text);
    }
}

TEST_CASE("seconds counts down in whole seconds, minutes from 60", "[ui_pipes]") {
    UiPipes pipes;
    UiContext ctx;
    ctx.set_pipes(&pipes);
    const std::pair<float, const char*> cases[] = {
        {2.4f, "3"}, {1.0f, "1"}, {0.2f, "1"}, {0.0f, "0"}, {-1.0f, "0"}, {59.5f, "1:00"}, {125.0f, "2:05"},
    };
    for (const auto& [seconds, text] : cases) {
        ctx.set("left", UiValue(seconds));
        INFO(seconds << " s");
        REQUIRE(show("{left|seconds}", ctx) == text);
    }
}

TEST_CASE("A child scope formats with its UI's pipes", "[ui_pipes]") {
    UiPipes pipes;
    UiContext root;
    root.set_pipes(&pipes);
    UiContext widget(&root);
    widget.set("hp", UiValue(71.6f));
    REQUIRE(show("{hp|round}", widget) == "72");
}

TEST_CASE("A game pipe is called with its declared types", "[ui_pipes]") {
    static const FjellFieldType args[] = {FJELL_FIELD_STD_STRING};
    const FjellPipeDesc table[] = {
        {"coins", FJELL_FIELD_INT, args, 1,
         [](const void* input, const void* const* arg, void* out) {
             *static_cast<std::string*>(out) = std::to_string(*static_cast<const int*>(input) / 100) +
                                               *static_cast<const std::string*>(arg[0]);
         }},
        {"round", FJELL_FIELD_FLOAT, nullptr, 0,
         [](const void*, const void* const*, void* out) { *static_cast<std::string*>(out) = "game"; }},
    };
    ReflectedTypes types;
    types.add_pipes(table, 2);
    UiPipes pipes;
    pipes.set_game_pipes(&types);
    UiContext ctx;
    ctx.set_pipes(&pipes);
    ctx.set("copper", UiValue(1250.0f)); // a float value, handed to an int parameter

    REQUIRE(show("{copper|coins:' gold'}", ctx) == "12 gold");
    REQUIRE(show("{copper|coins}", ctx) == "1250");         // missing argument: left as it is
    REQUIRE(show("{copper|silver}", ctx) == "1250");        // no such pipe: left as it is
    REQUIRE(show("{copper|round}", ctx) == "1250");         // the engine's round wins
}

TEST_CASE("A prop bound through a pipe takes its declared type", "[ui_pipes]") {
    auto root = std::make_unique<UiNode>();
    root->type = UiNodeType::element;
    auto widget = std::make_unique<UiNode>();
    widget->type = UiNodeType::element;
    widget->is_widget = true;
    widget->scoped_context = std::make_unique<UiContext>();
    widget->widget_props = {{"shown", "int", "0"}};
    widget->prop_map["shown"] = "{stamina|round}";
    auto* placed = widget.get();
    root->children.push_back(std::move(widget));

    UiPipes pipes;
    UiContext ctx;
    ctx.set_pipes(&pipes);
    ctx.set("stamina", UiValue(117.6f));
    UiDataBind bind;
    bind.resolve(*root, ctx);

    REQUIRE(*placed->scoped_context->find("shown") == UiValue(118));
}
