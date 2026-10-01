#include "ftk/ui/kit/field.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/pages.hpp"
#include "ftk/ui/kit/row.hpp"
#include "ftk/ui/kit/save_bar.hpp"
#include "ftk/ui/kit/section.hpp"
#include "imgui_harness.hpp"

#include <catch2/catch_test_macros.hpp>

using fjell::test::ImGuiHarness;
namespace ui = fjell::ui;

namespace {

// A small settings page: a folding group of two rows with a hint.
struct SettingsPage {
    bool bloom{true};
    bool gtao{true};
    int rows_drawn{0};
    bool hint_drawn{false};

    void draw() {
        rows_drawn = 0;
        hint_drawn = false;
        if (!ui::subheading_foldable("Post-processing", false)) return;
        if (auto t = ui::PropertyTable("##post")) {
            ui::row("Bloom", [&] { ++rows_drawn; (void)ui::checkbox("##bloom", bloom); });
            ui::row("GTAO", [&] { ++rows_drawn; (void)ui::checkbox("##gtao", gtao); });
            hint_drawn = true;
            ui::hint("Screen-space ambient occlusion");
        }
    }
};

} // namespace

TEST_CASE("A row filter keeps the rows whose label matches, inside folded groups", "[ui][kit]") {
    ImGuiHarness h;
    SettingsPage page;
    int matches = -1;
    std::string query = "bloo";
    h.set_ui([&] {
        const ui::RowFilter filter(query);
        page.draw();
        matches = filter.matches();
    });
    h.step(2);
    // The group starts folded, but a search opens every group.
    CHECK(page.rows_drawn == 1);
    CHECK(matches == 1);

    query.clear();
    h.step(2);
    // With no query nothing is filtered, and the folded group stays shut.
    CHECK(page.rows_drawn == 0);
}

TEST_CASE("A row filter drops hints, which belong to rows it may have left out", "[ui][kit]") {
    ImGuiHarness h;
    SettingsPage page;
    int matches = -1;
    h.set_ui([&] {
        const ui::RowFilter filter("colour");
        page.draw();
        matches = filter.matches();
    });
    h.step(2);
    CHECK(page.rows_drawn == 0);
    CHECK(matches == 0);
}

TEST_CASE("A page entry reports a click", "[ui][kit]") {
    ImGuiHarness h;
    int picked = -1;
    h.set_ui([&] {
        if (auto list = ui::PageList("##pages")) {
            if (ui::page_entry(ui::icon::rendering, "Rendering", picked == 0)) picked = 0;
            if (ui::page_entry(ui::icon::world_environment, "Environment", picked == 1)) picked = 1;
            h.mark("environment");
        }
    });
    h.step(2);
    h.click("environment");
    CHECK(picked == 1);
}

TEST_CASE("The save bar offers Save and Revert only while there are unsaved changes", "[ui][kit]") {
    ImGuiHarness h;
    bool unsaved = false;
    ui::SaveAction last = ui::SaveAction::None;
    h.set_ui([&] {
        const ui::SaveAction action = ui::save_bar(unsaved, "project.fjell");
        if (action != ui::SaveAction::None) last = action;
        h.mark("bar");
    });
    h.step(2);
    // Save sits against the bar's right edge.
    const ImVec2 save{h.rect_max("bar").x - 30.0f, (h.rect_min("bar").y + h.rect_max("bar").y) * 0.5f};
    ImGuiIO& io = ImGui::GetIO();
    auto click_at = [&](ImVec2 p) {
        io.AddMousePosEvent(p.x, p.y);
        h.step();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        h.step();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        h.step();
    };
    click_at(save);
    CHECK(last == ui::SaveAction::None);

    unsaved = true;
    h.step();
    click_at(save);
    CHECK(last == ui::SaveAction::Save);
}
