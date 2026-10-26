#include "kit_harness.hpp"

#include "ftk/ui/kit/dock.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/panel.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/window.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

// A window let go of past the edge of the OS window it is carried in: it
// floats in that window, against the edge it went past, its title bar in
// reach.

using Catch::Approx;
using ftk::test::KitHarness;
namespace ui = ftk::ui;

namespace {

// A dockspace of a hierarchy beside a viewport, each a panel.
void draw_docked(bool& built) {
    using N = ui::DockNode;
    const ui::Id dock = ui::id("##dock");
    ui::dockspace(dock);
    if (!built) {
        ui::build_dock_tree(dock, {800.0f, 600.0f},
                            N::left_right(0.5f, N::tabs({"Hierarchy"}), N::tabs({"Viewport"})));
        built = true;
    }
    for (const char* name : {"Hierarchy", "Viewport"}) {
        if (auto panel = ui::Panel("", name)) ui::text(name);
    }
}

// The middle of the tab of the window `name`.
glm::vec2 tab_of(const KitHarness& h, const char* name) {
    const ui::core::SemanticNode* tab = h.find(name, ui::core::Role::Tab);
    REQUIRE(tab != nullptr);
    return tab->rect.centre();
}

} // namespace

TEST_CASE("A tab carried past its window's edge floats inside it, against that edge", "[ui][kit][dock]") {
    KitHarness h;
    bool built = false;
    h.set_ui([&] { draw_docked(built); });
    h.step(4);

    // Off its row and out past the right edge of the harness's window.
    h.drag(tab_of(h, "Hierarchy"), {h.size().x + 100.0f, 300.0f});
    h.step(3);
    CHECK(h.windows().empty());
    CHECK(ui::window_dock("Hierarchy") == 0);
    const auto floating = ui::window_named("Hierarchy");
    REQUIRE(floating.has_value());
    CHECK(floating->pos.x >= 0.0f);
    CHECK(floating->pos.y >= 0.0f);
    CHECK(floating->pos.y + floating->size.y <= h.size().y);
    CHECK(floating->pos.x + floating->size.x == Approx(h.size().x));

    // Its title bar, in reach, carries it back onto the viewport's tabs.
    const glm::vec2 title{floating->pos.x + 40.0f, floating->pos.y + ui::frame_height() * 0.5f};
    h.drag(title, tab_of(h, "Viewport"));
    h.step(3);
    CHECK(ui::window_dock("Hierarchy") != 0);
    CHECK(ui::window_dock("Hierarchy") == ui::window_dock("Viewport"));
}

TEST_CASE("A floating window let go past its window's edge by its title bar stays inside, against that edge",
          "[ui][kit]") {
    KitHarness h;
    h.set_ui([&] {
        ui::next_window_size_once({240.0f, 160.0f});
        if (auto panel = ui::Panel("", "Notes")) ui::text("Notes");
    });
    h.step(3);
    const auto notes = ui::window_named("Notes");
    REQUIRE(notes.has_value());
    const glm::vec2 title{notes->pos.x + 40.0f, notes->pos.y + ui::frame_height() * 0.5f};
    h.drag(title, {title.x, h.size().y + 80.0f});
    h.step(3);
    CHECK(h.windows().empty());
    const auto after = ui::window_named("Notes");
    REQUIRE(after.has_value());
    CHECK(after->size == glm::vec2{240.0f, 160.0f});
    CHECK(after->pos.x == Approx(notes->pos.x));
    CHECK(after->pos.y + after->size.y == Approx(h.size().y));
}
