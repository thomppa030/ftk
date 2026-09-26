#include "scripting/reflected_types.hpp"
#include "ui/game_ui/ui_actions.hpp"
#include "ui/game_ui/ui_context.hpp"
#include "ui/game_ui/ui_data_bind.hpp"

#include <catch2/catch_test_macros.hpp>

#include <glm/vec2.hpp>

#include <string>
#include <vector>

using namespace fjell;

namespace {

// What a widget's [[fj::action]] methods received, as the generated shims
// would hand it over.
struct Probe {
    int calls{0};
    int whole{0};
    float number{0.0f};
    glm::vec2 point{0.0f};
};

OwnedUiActionDesc action(const char* name, FjellUiActionParam param, void (*invoke)(void*, const void*)) {
    OwnedUiActionDesc desc;
    desc.name = name;
    desc.param = param;
    desc.invoke = invoke;
    return desc;
}

const OwnedUiActionDesc CLOSE = action("close", FJELL_UI_ACTION_NONE, [](void* self, const void*) {
    ++static_cast<Probe*>(self)->calls;
});
const OwnedUiActionDesc SELECT = action("select_slot", FJELL_UI_ACTION_INT, [](void* self, const void* arg) {
    auto& probe = *static_cast<Probe*>(self);
    ++probe.calls;
    probe.whole = *static_cast<const int*>(arg);
});
const OwnedUiActionDesc SCALE = action("scale", FJELL_UI_ACTION_FLOAT, [](void* self, const void* arg) {
    auto& probe = *static_cast<Probe*>(self);
    ++probe.calls;
    probe.number = *static_cast<const float*>(arg);
});
const OwnedUiActionDesc AIM = action("aim", FJELL_UI_ACTION_VEC2, [](void* self, const void* arg) {
    auto& probe = *static_cast<Probe*>(self);
    ++probe.calls;
    probe.point = *static_cast<const glm::vec2*>(arg);
});

std::string call(const OwnedUiActionDesc& desc, Probe& probe, std::vector<UiValue> args) {
    return call_action(desc, &probe, args);
}

} // namespace

TEST_CASE("A call splits into its action and arguments", "[ui_actions]") {
    const auto call_with = parse_action_call("select_slot({slot.key})");
    REQUIRE(call_with.name == "select_slot");
    REQUIRE(call_with.args == std::vector<std::string>{"{slot.key}"});
    REQUIRE(parse_action_call("close").args.empty());
    REQUIRE(parse_action_call("close()").args.empty());
    REQUIRE(parse_action_call(" aim( 0.5 , 1 ) ").args == std::vector<std::string>{"0.5", "1"});
}

TEST_CASE("An action gets its argument as the type it takes", "[ui_actions]") {
    Probe probe;
    REQUIRE(call(CLOSE, probe, {}).empty());
    REQUIRE(probe.calls == 1);

    REQUIRE(call(SELECT, probe, {UiValue(3)}).empty());
    REQUIRE(probe.whole == 3);
    REQUIRE(call(SELECT, probe, {UiValue(2.9f)}).empty()); // a whole number, cut
    REQUIRE(probe.whole == 2);
    REQUIRE(call(SELECT, probe, {UiValue("7")}).empty()); // text that is a number
    REQUIRE(probe.whole == 7);

    REQUIRE(call(SCALE, probe, {UiValue("0.5")}).empty());
    REQUIRE(probe.number == 0.5f);

    REQUIRE(call(AIM, probe, {UiValue(glm::vec2(1.0f, 2.0f))}).empty());
    REQUIRE(probe.point == glm::vec2(1.0f, 2.0f));
    REQUIRE(call(AIM, probe, {UiValue(3), UiValue("4")}).empty()); // or two numbers
    REQUIRE(probe.point == glm::vec2(3.0f, 4.0f));
    REQUIRE(probe.calls == 7);
}

TEST_CASE("An argument that does not fit is refused and says why", "[ui_actions]") {
    Probe probe;
    const std::string why = call(SELECT, probe, {UiValue("abc")});
    REQUIRE(why.find("select_slot takes a whole number") != std::string::npos);
    REQUIRE(why.find("'abc'") != std::string::npos);
    REQUIRE_FALSE(call(CLOSE, probe, {UiValue(1)}).empty());  // takes nothing
    REQUIRE_FALSE(call(SELECT, probe, {}).empty());           // takes one
    REQUIRE_FALSE(call(AIM, probe, {UiValue(1)}).empty());    // a number is no vec2
    REQUIRE(probe.calls == 0);

    OwnedUiActionDesc stale = SELECT;
    stale.invoke = nullptr;
    REQUIRE(call(stale, probe, {UiValue(1)}).find("rebuild") != std::string::npos);
}

TEST_CASE("A call is checked before it runs as far as its literals go", "[ui_actions]") {
    REQUIRE(check_action_call(SELECT, parse_action_call("select_slot(3)")).empty());
    REQUIRE(check_action_call(SELECT, parse_action_call("select_slot({slot.key})")).empty()); // judged when clicked
    REQUIRE_FALSE(check_action_call(SELECT, parse_action_call("select_slot(three)")).empty());
    REQUIRE_FALSE(check_action_call(SELECT, parse_action_call("select_slot(1, 2)")).empty());
    REQUIRE_FALSE(check_action_call(CLOSE, parse_action_call("close(1)")).empty());
    REQUIRE(check_action_call(AIM, parse_action_call("aim(1, 2)")).empty());
    REQUIRE(check_action_call(AIM, parse_action_call("aim({target})")).empty());
    REQUIRE_FALSE(check_action_call(AIM, parse_action_call("aim(1)")).empty());
}

TEST_CASE("An action is answered by the nearest scope that takes it", "[ui_actions]") {
    std::vector<std::string> heard;
    UiContext root;
    UiContext widget(&root);
    root.set_action_handler([&](std::string_view action, std::span<const UiValue>) {
        if (action != "close") return false;
        heard.push_back("root:close");
        return true;
    });
    widget.set_action_handler([&](std::string_view action, std::span<const UiValue> args) {
        if (action != "select_slot") return false;
        heard.push_back("widget:select_slot " + args[0].text());
        return true;
    });

    const UiValue three(3);
    REQUIRE(widget.dispatch("select_slot", std::span(&three, 1)));
    REQUIRE(widget.dispatch("close")); // bubbles up
    REQUIRE_FALSE(widget.dispatch("jump"));
    REQUIRE(heard == std::vector<std::string>{"widget:select_slot 3", "root:close"});
}

TEST_CASE("A click's argument hands over the value it reads", "[ui_actions]") {
    UiContext ctx;
    ctx.set("key", UiValue(5));
    REQUIRE(UiDataBind::resolve_value("{key}", ctx) == UiValue(5)); // the int itself
    REQUIRE(UiDataBind::resolve_value("slot {key}", ctx) == UiValue("slot 5"));
    REQUIRE(UiDataBind::resolve_value("4", ctx) == UiValue("4"));
}
