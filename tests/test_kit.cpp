#include "imgui_harness.hpp"
#include "ui/kit/button.hpp"
#include "ui/kit/section.hpp"

#include <catch2/catch_test_macros.hpp>

using fjell::test::ImGuiHarness;
namespace ui = fjell::ui;

TEST_CASE("Every button kind reports its click", "[ui][kit]") {
    const ui::ButtonKind kinds[] = {ui::ButtonKind::Secondary, ui::ButtonKind::Primary,
                                    ui::ButtonKind::Ghost, ui::ButtonKind::Danger,
                                    ui::ButtonKind::GhostDanger};
    for (auto kind : kinds) {
        ImGuiHarness h;
        int clicks = 0;
        h.set_ui([&] {
            if (ui::button("Save", kind)) ++clicks;
            h.mark("button");
        });
        h.step(2);
        h.click("button");
        CHECK(clicks == 1);
    }
}

TEST_CASE("An icon button is a frame-height square", "[ui][kit]") {
    ImGuiHarness h;
    int clicks = 0;
    h.set_ui([&] {
        if (ui::icon_button("clear", "x", "Clear")) ++clicks;
        h.mark("icon");
    });
    h.step(2);
    const ImVec2 min = h.rect_min("icon");
    const ImVec2 max = h.rect_max("icon");
    CHECK(max.x - min.x == max.y - min.y);
    h.click("icon");
    CHECK(clicks == 1);
}

TEST_CASE("A toggle reports clicks and leaves the state to the caller", "[ui][kit]") {
    ImGuiHarness h;
    bool on = false;
    h.set_ui([&] {
        if (ui::toggle_button("snap", "M", on, "Snap to grid")) on = !on;
        h.mark("toggle");
    });
    h.step(2);
    h.click("toggle");
    CHECK(on);
    h.click("toggle");
    CHECK_FALSE(on);
}

TEST_CASE("A foldable section opens and closes and remembers", "[ui][kit]") {
    ImGuiHarness h;
    bool open = false;
    h.set_ui([&] {
        open = ui::section_foldable("Weather", nullptr, true);
        h.mark("heading");
    });
    h.step(2);
    CHECK(open);
    h.click("heading");
    CHECK_FALSE(open);
    h.step(3);
    CHECK_FALSE(open);
    h.click("heading");
    CHECK(open);
}

TEST_CASE("A foldable sub-heading starts the way it is asked to", "[ui][kit]") {
    ImGuiHarness h;
    bool open = true;
    h.set_ui([&] {
        open = ui::subheading_foldable("Lightning", false);
        h.mark("heading");
    });
    h.step(2);
    CHECK_FALSE(open);
    h.click("heading");
    CHECK(open);
}
