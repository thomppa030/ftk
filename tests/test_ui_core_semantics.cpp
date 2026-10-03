#include "ftk/test/ui_harness.hpp"
#include "ftk/ui/core/context.hpp"

#include <catch2/catch_test_macros.hpp>

#include <glm/vec2.hpp>

#include <string>

namespace core = ftk::ui::core;
using core::Role;
using ftk::test::UiHarness;
using ftk::ui::Id;

namespace {

// A button that says what it is, as the kit's will.
bool button(UiHarness& h, const char* label) {
    core::Context& ui = h.ui();
    const Id id = ui.id(label);
    const core::Interaction in = ui.interact(id, ui.layout().next({100.0f, 24.0f}));
    ui.semantics(id, Role::Button, label);
    return in.clicked;
}

} // namespace

TEST_CASE("Each widget records its role, label, value and state as it is drawn", "[ui-core][semantics]") {
    UiHarness h;
    h.set_ui([&] {
        core::Context& ui = h.ui();
        (void)button(h, "Save##scene");
        const Id opacity = ui.id("Opacity");
        (void)ui.interact(opacity, ui.layout().next({200.0f, 24.0f}));
        ui.semantics(opacity, Role::Slider, "Opacity", "0.75");
        ui.push_disabled();
        (void)button(h, "Bake");
        ui.pop_disabled();
        const Id visible = ui.id("Visible");
        (void)ui.interact(visible, ui.layout().next({24.0f, 24.0f}));
        ui.semantics(visible, Role::Toggle, "Visible", {}, {.checked = true});
    });
    h.step();
    const auto records = h.ui().semantics();
    REQUIRE(records.size() == 4);
    CHECK(records[0].label == "Save");
    CHECK(records[0].role == Role::Button);
    CHECK(records[0].rect.min == glm::vec2{8.0f, 8.0f});
    CHECK(records[1].value == "0.75");
    CHECK(records[2].state.disabled);
    CHECK(records[3].state.checked);
    CHECK_FALSE(records[3].state.disabled);
    CHECK(records[1].root == 1);

    // Found by name, and by role where names repeat.
    REQUIRE(h.find("Opacity") != nullptr);
    CHECK(h.find("Opacity")->role == Role::Slider);
    CHECK(h.find("Opacity", Role::Button) == nullptr);
    CHECK(h.find("Nothing") == nullptr);
}

TEST_CASE("The harness clicks a widget by its name, with no mark", "[ui-core][semantics]") {
    UiHarness h;
    int saved = 0;
    int loaded = 0;
    h.set_ui([&] {
        saved += button(h, "Save") ? 1 : 0;
        loaded += button(h, "Load") ? 1 : 0;
    });
    h.step();
    h.click("Load");
    h.click("Save");
    h.click("Save");
    CHECK(saved == 2);
    CHECK(loaded == 1);
}

TEST_CASE("A record knows whether its widget has the keyboard", "[ui-core][semantics]") {
    UiHarness h;
    h.set_ui([&] {
        core::Context& ui = h.ui();
        const Id name = ui.id("Name");
        (void)ui.interact(name, ui.layout().next({200.0f, 24.0f}));
        (void)ui.focusable(name, {.text = true});
        ui.semantics(name, Role::TextField, "Name", "Crate");
    });
    h.step();
    CHECK_FALSE(h.find("Name")->state.focused);
    h.click("Name");
    h.step();
    CHECK(h.find("Name")->state.focused);
    CHECK(h.find("Name")->value == "Crate");
}

TEST_CASE("What a container hides for its first frame isn't recorded until it shows", "[ui-core][semantics]") {
    UiHarness h;
    h.set_ui([&] {
        h.ui().begin_container("bar");
        (void)button(h, "Inside");
        h.ui().end_container();
    });
    h.step();
    CHECK(h.find("Inside") == nullptr);
    h.step();
    CHECK(h.find("Inside") != nullptr);
}

TEST_CASE("The last frame's records hold while the next is drawn", "[ui-core][semantics]") {
    UiHarness h;
    int frame = 0;
    std::string seen;
    h.set_ui([&] {
        ++frame;
        // Read the last frame's while this one records its own.
        if (const core::SemanticNode* last = h.ui().find("Counter")) seen = std::string(last->value);
        const Id counter = h.ui().id("Counter");
        h.ui().semantics(counter, Role::Text, "Counter", std::to_string(frame), {}, {});
    });
    h.step(3);
    CHECK(seen == "2");
    CHECK(h.find("Counter")->value == "3");
}
