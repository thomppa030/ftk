#include "kit_harness.hpp"

#include "ftk/ui/kit/dock.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/panel.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/window.hpp"

#include <catch2/catch_test_macros.hpp>

// Tearing a window out into an OS window of its own (decision 20 of the UI
// core plan), on the kit drawn by the core: the ImGui kit's windows stay in
// the program's window.

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

TEST_CASE("A tab carried past its window's edge opens a window of its own, and docks back", "[ui][kit][dock]") {
    KitHarness h;
    bool built = false;
    h.set_ui([&] { draw_docked(built); });
    h.step(4);

    // Off its row and out past the right edge of the harness's window.
    h.drag(tab_of(h, "Hierarchy"), {h.size().x + 100.0f, 300.0f});
    h.step(3);
    REQUIRE(h.windows().size() == 1);
    const ui::core::RootRequest torn = h.windows()[0];
    CHECK(torn.title == "Hierarchy");
    CHECK(torn.near == KitHarness::WINDOW);
    CHECK(torn.size.x > 0.0f);
    CHECK(ui::window_dock("Hierarchy") == 0);
    // Drawn in that window, filling it.
    const auto in_window = ui::window_named("Hierarchy");
    REQUIRE(in_window.has_value());
    CHECK(in_window->pos == glm::vec2{0.0f});
    CHECK(in_window->size == torn.size);
    REQUIRE(h.output().roots.size() == 2);
    CHECK(h.output().roots[1].root == torn.root);
    CHECK_FALSE(h.output().roots[1].draw->primitives.empty());

    // The button at the right end of its row docks it back, where the leaf
    // it left folded, and its window closes.
    const float bar = ui::frame_height();
    h.click_in(torn.root, {torn.size.x - bar * 0.5f, bar * 0.5f});
    h.step(2);
    CHECK(h.windows().empty());
    CHECK(ui::window_dock("Hierarchy") != 0);
    CHECK(ui::window_dock("Hierarchy") == ui::window_dock("Viewport"));

    // Torn out again, then its window closed by its user: docked back too.
    h.drag(tab_of(h, "Hierarchy"), {h.size().x + 100.0f, 300.0f});
    h.step(3);
    REQUIRE(h.windows().size() == 1);
    CHECK(ui::window_dock("Hierarchy") == 0);
    h.close_window(h.windows()[0].root);
    h.step(2);
    CHECK(h.windows().empty());
    CHECK(ui::window_dock("Hierarchy") == ui::window_dock("Viewport"));
}

TEST_CASE("A floating window let go past its window's edge by its title bar opens a window of its own", "[ui][kit]") {
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
    REQUIRE(h.windows().size() == 1);
    CHECK(h.windows()[0].size == glm::vec2{240.0f, 160.0f});
    // With no dockspace to go back to, it floats in the program's window.
    h.close_window(h.windows()[0].root);
    h.step(2);
    const auto back = ui::window_named("Notes");
    REQUIRE(back.has_value());
    CHECK(back->pos.y < h.size().y);
}
