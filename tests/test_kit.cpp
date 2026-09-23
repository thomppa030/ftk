#include "imgui_harness.hpp"
#include "ui/kit/button.hpp"
#include "ui/kit/section.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>

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

TEST_CASE("An icon button centres an icon wider than its padding leaves room for", "[ui][kit]") {
    ImGuiHarness h;
    h.set_ui([&] {
        // A wide label stands in for an icon glyph: wider than the square
        // minus ImGui's frame padding on both sides.
        ui::icon_button("wide", "WW", "Wide");
        h.mark("icon");
    });
    h.step(2);
    const ImVec2 min = h.rect_min("icon");
    const ImVec2 max = h.rect_max("icon");
    REQUIRE(ImGui::CalcTextSize("WW").x > (max.x - min.x) - ImGui::GetStyle().FramePadding.x * 2.0f);

    // Around the button, the text is the only thing drawn from the font
    // atlas rather than its white pixel, so its vertices give where the
    // label landed.
    const ImVec2 white = ImGui::GetIO().Fonts->TexUvWhitePixel;
    float text_min = FLT_MAX;
    float text_max = -FLT_MAX;
    const ImDrawData* data = ImGui::GetDrawData();
    for (const ImDrawList* list : data->CmdLists) {
        for (const ImDrawVert& v : list->VtxBuffer) {
            if (v.uv.x == white.x && v.uv.y == white.y) continue;
            if (v.pos.y < min.y || v.pos.y > max.y) continue;
            if (v.pos.x < min.x - 16.0f || v.pos.x > max.x + 16.0f) continue;
            text_min = std::min(text_min, v.pos.x);
            text_max = std::max(text_max, v.pos.x);
        }
    }
    REQUIRE(text_min < text_max);
    const float text_centre = (text_min + text_max) * 0.5f;
    CHECK(std::abs(text_centre - (min.x + max.x) * 0.5f) <= 1.0f);
}
