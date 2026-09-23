#include "imgui_harness.hpp"
#include "ui/kit/asset_kind.hpp"
#include "ui/kit/button.hpp"
#include "ui/kit/component_block.hpp"
#include "ui/kit/feedback.hpp"
#include "ui/kit/field.hpp"
#include "ui/kit/menu.hpp"
#include "ui/kit/section.hpp"

#include <catch2/catch_test_macros.hpp>
#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string>

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

TEST_CASE("A component block folds from its header and asks to be removed from its trash icon", "[ui][kit]") {
    ImGuiHarness h;
    bool open = false;
    bool remove = false;
    int removals = 0;
    h.set_ui([&] {
        remove = false;
        open = ui::component_block("Collider", fjell::theme::Category::Physics, remove);
        h.mark("trash");  // the last item is the trash icon
        if (remove) ++removals;
    });
    h.step(2);
    CHECK(open);
    const ImVec2 trash_min = h.rect_min("trash");
    const ImVec2 trash_max = h.rect_max("trash");
    const float y = (trash_min.y + trash_max.y) * 0.5f;

    // The header left of the trash icon folds the block.
    ImGuiIO& io = ImGui::GetIO();
    const auto click_at = [&](float x) {
        io.AddMousePosEvent(x, y);
        h.step();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        h.step();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        h.step();
    };
    click_at(trash_min.x - 40.0f);
    CHECK_FALSE(open);
    click_at(trash_min.x - 40.0f);
    CHECK(open);

    // The trash icon asks for removal and leaves the fold alone.
    h.click("trash");
    CHECK(removals == 1);
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

TEST_CASE("An empty state sits in the middle of the space and offers its action", "[ui][kit]") {
    ImGuiHarness h;
    int clicks = 0;
    ImVec2 area_min;
    ImVec2 area_max;
    h.set_ui([&] {
        area_min = ImGui::GetCursorScreenPos();
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        area_max = {area_min.x + avail.x, area_min.y + avail.y};
        if (ui::empty_state("?", "Nothing selected", "Pick an object to edit it here", "Select all")) {
            ++clicks;
        }
        h.mark("action");
    });
    h.step(2);
    const ImVec2 min = h.rect_min("action");
    const ImVec2 max = h.rect_max("action");
    CHECK(std::abs((min.x + max.x) * 0.5f - (area_min.x + area_max.x) * 0.5f) <= 1.0f);
    // The block is centred vertically, so its last line is below the middle
    // but nowhere near the bottom.
    CHECK(max.y > (area_min.y + area_max.y) * 0.5f);
    CHECK(max.y < area_max.y - 100.0f);
    h.click("action");
    CHECK(clicks == 1);
}

TEST_CASE("An asset's kind comes from its extension, by domain", "[ui][kit]") {
    using fjell::theme::Category;
    CHECK(ui::asset_kind(".fjmat").category == Category::Rendering);
    CHECK(std::string(ui::asset_kind(".fjmat").noun) == "material");
    CHECK(ui::asset_kind(".png").category == Category::Rendering);
    CHECK(ui::asset_kind(".fjweather").category == Category::Environment);
    CHECK(ui::asset_kind(".fjsurface").category == Category::Physics);
    CHECK(ui::asset_kind(".fjanim").category == Category::Animation);
    CHECK(std::string(ui::asset_kind(".xyz").noun) == "file");
}

TEST_CASE("A disabled icon button still shows its tooltip", "[ui][kit]") {
    ImGuiHarness h;
    h.set_ui([&] {
        ImGui::BeginDisabled(true);
        ui::icon_button("clear", "x", "Nothing to clear");
        ImGui::EndDisabled();
        h.mark("button");
    });
    h.step(2);
    const ImVec2 min = h.rect_min("button");
    const ImVec2 max = h.rect_max("button");
    ImGui::GetIO().AddMousePosEvent((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
    // Past the tooltip delay.
    h.step(30);
    const ImGuiWindow* tooltip = ImGui::FindWindowByName("##Tooltip_00");
    REQUIRE(tooltip != nullptr);
    CHECK(tooltip->WasActive);
}

TEST_CASE("A menu entry reports its click, and a disabled one doesn't", "[ui][kit]") {
    ImGuiHarness h;
    int fits = 0;
    int creates = 0;
    h.set_ui([&] {
        if (ui::menu_item({.label = "Fit to mesh"})) ++fits;
        h.mark("fit");
        if (ui::menu_item({.label = "Create material", .enabled = false,
                           .disabled_reason = "Needs a terrain asset first"})) {
            ++creates;
        }
        h.mark("create");
    });
    h.step(2);
    h.click("fit");
    h.click("create");
    CHECK(fits == 1);
    CHECK(creates == 0);
}

TEST_CASE("A tool button is a tool-sized square and reports its click", "[ui][kit]") {
    ImGuiHarness h;
    bool on = false;
    h.set_ui([&] {
        if (ui::tool_button("raise", "R", on, "Raise")) on = !on;
        h.mark("tool");
    });
    h.step(2);
    CHECK(h.rect_max("tool").x - h.rect_min("tool").x == fjell::theme::TOOL_BUTTON);
    CHECK(h.rect_max("tool").y - h.rect_min("tool").y == fjell::theme::TOOL_BUTTON);
    h.click("tool");
    CHECK(on);
}
