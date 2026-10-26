#include "kit_harness.hpp"

#include "ftk/ui/kit/asset_header.hpp"
#include "ftk/ui/kit/dock.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/other_window.hpp"
#include "ftk/ui/kit/panel.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/tabs.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/viewport_toolbar.hpp"
#include "ftk/ui/kit/window.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <optional>
#include <string>

// A window of the program's own beside the one being drawn, which the
// program fills (an editor's tab torn out into one), and the document tab
// that is torn off to make one.

using Catch::Approx;
using ftk::test::KitHarness;
namespace ui = ftk::ui;

namespace {

constexpr const char* OTHER = "##other";

// What a frame drew into the window `root`, if it drew it.
const ui::core::RootOutput* drawn(const KitHarness& h, ui::core::RootId root) {
    const auto& roots = h.output().roots;
    const auto found = std::ranges::find(roots, root, &ui::core::RootOutput::root);
    return found == roots.end() ? nullptr : &*found;
}

} // namespace

TEST_CASE("A window of the program's own is asked for, drawn into, closed by its user and by the program",
          "[ui][kit][other_window]") {
    KitHarness h;
    bool wanted = true;
    bool take_back = false;
    bool drawn_in = false;
    bool told_closed = false;
    h.set_ui([&] {
        if (take_back) {
            ui::close_other_window(OTHER);
            take_back = false;
            wanted = false;
        }
        if (!wanted) return;
        const ui::OtherWindow other(OTHER, {.title = "bark.fjmat", .size = {400.0f, 300.0f}, .at = {20.0f, 30.0f}});
        if (other) {
            if (auto panel = ui::Panel("", "Inside")) ui::text("inside");
            drawn_in = true;
        }
        if (other.closed()) {
            told_closed = true;
            wanted = false;
        }
    });

    // Asked for the first frame, a window of its own beside the harness's.
    h.step();
    REQUIRE(h.windows().size() == 1);
    const ui::core::RootRequest asked = h.windows()[0];
    CHECK(asked.root == ui::other_window_root(OTHER));
    CHECK(asked.kind == ui::core::RootKind::Window);
    CHECK(asked.title == "bark.fjmat");
    CHECK(asked.size == glm::vec2{400.0f, 300.0f});
    CHECK(asked.at == glm::vec2{20.0f, 30.0f});
    CHECK(asked.near == KitHarness::WINDOW);
    CHECK_FALSE(drawn_in);

    // Drawn into once the program has opened it: what it holds, from the
    // frame after.
    h.step(2);
    CHECK(drawn_in);
    h.step();
    const ui::core::RootOutput* inside = drawn(h, asked.root);
    REQUIRE(inside != nullptr);
    CHECK_FALSE(inside->draw->primitives.empty());

    // Its user closes it: the program is told once, and it is gone.
    h.close_window(asked.root);
    h.step();
    CHECK(told_closed);
    h.step();
    CHECK(h.windows().empty());

    // Drawn again, it opens anew; taken back by the program, it closes.
    wanted = true;
    h.step();
    CHECK(h.windows().size() == 1);
    take_back = true;
    h.step(2);
    CHECK(h.windows().empty());
}

TEST_CASE("A window of the program's own takes its title as it changes, and is raised when asked",
          "[ui][kit][other_window]") {
    KitHarness h;
    std::string title = "bark.fjmat";
    bool raise = false;
    h.set_ui([&] {
        ui::OtherWindow other(OTHER, {.title = title, .size = {400.0f, 300.0f}});
        if (raise) other.raise();
    });
    h.step(3);
    REQUIRE(h.windows().size() == 1);
    CHECK(h.output().raise.empty());

    title = "bark.fjmat ●";
    h.step();
    REQUIRE(h.windows().size() == 1);
    CHECK(h.windows()[0].title == "bark.fjmat ●");
    CHECK(h.windows()[0].size == glm::vec2{400.0f, 300.0f});

    raise = true;
    h.step();
    CHECK(std::ranges::contains(h.output().raise, ui::other_window_root(OTHER)));
}

TEST_CASE("Each window has bars and an asset header of its own", "[ui][kit][other_window]") {
    KitHarness h;
    std::string main_header;
    std::string other_header;
    float other_work_top = -1.0f;
    const ui::AssetHeaderSpec spec{.name = "bark.fjmat"};
    h.set_ui([&] {
        { auto bar = ui::ContextTabBar("##bar"); }
        {
            auto header = ui::AssetHeaderBar(spec);
            main_header = ui::asset_header_window();
        }
        const ui::OtherWindow other(OTHER, {.title = "bark.fjmat", .size = {400.0f, 300.0f}});
        if (!other) return;
        { auto bar = ui::ContextTabBar("##other_bar"); }
        {
            auto header = ui::AssetHeaderBar(spec);
            other_header = ui::asset_header_window();
        }
        other_work_top = ui::work_area().pos.y;
    });
    h.step(4);

    // The other window's bars start at its own top, not under the main
    // window's.
    const auto bar = ui::window_named("##bar");
    const auto other_bar = ui::window_named("##other_bar");
    REQUIRE(bar.has_value());
    REQUIRE(other_bar.has_value());
    CHECK(other_bar->pos.y == Approx(0.0f));
    CHECK(other_bar->size.y == Approx(bar->size.y));

    // Its header is its own window, under its own tab bar.
    CHECK(main_header != other_header);
    const auto header = ui::window_named(main_header);
    const auto other_header_shown = ui::window_named(other_header);
    REQUIRE(header.has_value());
    REQUIRE(other_header_shown.has_value());
    CHECK(other_header_shown->pos.y == Approx(header->pos.y));
    CHECK(other_work_top == Approx(header->pos.y + header->size.y));
}

TEST_CASE("A window carried in one OS window never docks into another's dockspace", "[ui][kit][other_window][dock]") {
    using N = ui::DockNode;
    KitHarness h;
    bool built = false;
    h.set_ui([&] {
        ui::next_window_size_once({200.0f, 120.0f});
        if (auto panel = ui::Panel("", "Notes")) ui::text("notes");
        const ui::OtherWindow other(OTHER, {.title = "rows", .size = {400.0f, 300.0f}});
        if (!other) return;
        const ui::Id dock = ui::id("##other_dock");
        ui::next_window_place({0.0f, 0.0f}, {400.0f, 300.0f});
        (void)ui::begin_host_window("##other_host",
                                    {.background = false, .docking = false, .to_front = false, .nav_focus = false});
        ui::dockspace(dock);
        if (!built) {
            ui::build_dock_tree(dock, {400.0f, 300.0f}, N::tabs({"Rows"}));
            built = true;
        }
        ui::end_host_window();
        if (auto panel = ui::Panel("", "Rows")) ui::text("rows");
    });
    h.step(4);
    REQUIRE(ui::window_dock("Rows") != 0);

    // Notes, carried in the harness's window to where the other window's
    // tabs are in its own, stays floating.
    const ui::core::SemanticNode* rows_tab = h.find("Rows", ui::core::Role::Tab);
    REQUIRE(rows_tab != nullptr);
    const auto notes = ui::window_named("Notes");
    REQUIRE(notes.has_value());
    const glm::vec2 title{notes->pos.x + 40.0f, notes->pos.y + ui::frame_height() * 0.5f};
    h.drag(title, rows_tab->rect.centre());
    h.step(3);
    CHECK(ui::window_dock("Notes") == 0);
}

TEST_CASE("A document tab that tears off says where it was let go past the window's edge", "[ui][kit][tabs]") {
    KitHarness h;
    std::optional<glm::vec2> torn;
    bool other_torn = false;
    h.set_ui([&] {
        if (auto tabs = ui::DocumentTabs("##docs")) {
            (void)ui::document_tab({.label = "Scene", .closable = false});
            const ui::DocumentTabResult bark = ui::document_tab({.label = "bark.fjmat", .tear_off = true});
            if (bark.torn_off) torn = bark.torn_off;
            const ui::DocumentTabResult stone = ui::document_tab({.label = "stone.fjmesh"});
            if (stone.torn_off) other_torn = true;
        }
    });
    h.step(3);
    const auto centre = [&](const char* name) {
        const ui::core::SemanticNode* tab = h.find(name, ui::core::Role::Tab);
        REQUIRE(tab != nullptr);
        return tab->rect.centre();
    };

    // Dragged off its row, then past the next tab's middle, and let go inside
    // the window: it keeps its place, and nothing is torn off.
    const glm::vec2 bark = centre("bark.fjmat");
    const glm::vec2 stone = centre("stone.fjmesh");
    h.move_to(bark);
    h.button_down();
    h.move_to({bark.x, bark.y + 100.0f});
    h.move_to({bark.x, bark.y + 200.0f});
    h.move_to({stone.x + 20.0f, bark.y + 200.0f});
    h.button_up();
    h.step(2);
    CHECK_FALSE(torn.has_value());
    CHECK(centre("bark.fjmat").x < centre("stone.fjmesh").x);

    // Let go past the window's bottom edge: torn off, there.
    h.drag(centre("bark.fjmat"), {bark.x + 10.0f, h.size().y + 50.0f});
    h.step(2);
    REQUIRE(torn.has_value());
    CHECK(torn->y > h.size().y);

    // A tab that doesn't tear off, let go there, is not.
    h.drag(centre("stone.fjmesh"), {stone.x, h.size().y + 50.0f});
    h.step(2);
    CHECK_FALSE(other_torn);
}
