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
#include <vector>

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

TEST_CASE("A window of the program's own its user asks to close stays until the program closes it",
          "[ui][kit][other_window]") {
    KitHarness h;
    bool wanted = true;
    bool take_back = false;
    int asks = 0;
    h.set_ui([&] {
        if (take_back) {
            ui::close_other_window(OTHER);
            take_back = false;
            wanted = false;
        }
        if (!wanted) return;
        const ui::OtherWindow other(OTHER, {.title = "bark.fjmat", .size = {400.0f, 300.0f}});
        if (other.close_asked()) ++asks;
    });
    h.step(3);
    REQUIRE(h.windows().size() == 1);

    // Asked, as unsaved work in it would be asked about there: still open,
    // and told once.
    h.ask_close_window(h.windows()[0].root);
    h.step(3);
    CHECK(asks == 1);
    CHECK(h.windows().size() == 1);

    // The program closes it.
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
    // As a program that opens popups past its window's edge has them.
    h.ui().set_popups_leave_window(true);
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
    // Its copy at the pointer is drawn inside the window.
    CHECK(h.windows().empty());
    h.button_up();
    h.step(2);
    CHECK_FALSE(torn.has_value());
    CHECK(centre("bark.fjmat").x < centre("stone.fjmesh").x);

    // Past the window's bottom edge its copy says what letting go does, and
    // let go there it is torn off, there.
    h.move_to(centre("bark.fjmat"));
    h.button_down();
    h.move_to({bark.x, bark.y + 200.0f});
    h.move_to({bark.x + 10.0f, h.size().y + 50.0f});
    h.step(2);
    // Past the edge its copy is in a window of its own that takes no
    // pointers, wider than the tab by what it says there.
    REQUIRE(h.windows().size() == 1);
    CHECK(h.windows()[0].kind == ui::core::RootKind::Tooltip);
    const ui::core::SemanticNode* bark_tab = h.find("bark.fjmat", ui::core::Role::Tab);
    REQUIRE(bark_tab != nullptr);
    CHECK(h.windows()[0].size.x > bark_tab->rect.width() + 100.0f);
    h.button_up();
    h.step(2);
    REQUIRE(torn.has_value());
    CHECK(torn->y > h.size().y);
    CHECK(h.windows().empty());

    // A tab that doesn't tear off, let go there, is not.
    h.drag(centre("stone.fjmesh"), {stone.x, h.size().y + 50.0f});
    h.step(2);
    CHECK_FALSE(other_torn);
}

TEST_CASE("A tab dragged onto another window's row of its group parts it, and lands where it parted",
          "[ui][kit][tabs][other_window]") {
    KitHarness h;
    h.ui().set_popups_leave_window(true);
    std::vector<std::string> here{"Scene", "bark.fjmat"};
    std::vector<std::string> there{"grass.fjmat", "stone.fjmesh"};
    std::optional<int> landed;
    std::optional<glm::vec2> torn;
    std::vector<std::size_t> places_there;
    const auto row = [&](const std::vector<std::string>& tabs, int owner) {
        const std::string id = "##docs " + std::to_string(owner);
        if (auto docs = ui::DocumentTabs(id.c_str(), {.group = "editor", .owner = owner})) {
            for (const std::string& name : tabs) {
                const ui::DocumentTabResult tab = ui::document_tab({.label = name.c_str(), .tear_off = name != "Scene"});
                if (tab.landed_on) landed = tab.landed_on;
                if (tab.torn_off) torn = tab.torn_off;
                if (owner == 7) places_there.push_back(tab.place);
            }
        }
    };
    h.set_ui([&] {
        places_there.clear();
        row(here, 0);
        const ui::OtherWindow other(OTHER, {.title = "other", .size = {500.0f, 300.0f}});
        if (other) row(there, 7);
    });
    h.step(3);
    const ui::core::RootId other = ui::other_window_root(OTHER);
    const auto tab_rect = [&](const char* name) {
        const ui::core::SemanticNode* tab = h.find(name, ui::core::Role::Tab);
        REQUIRE(tab != nullptr);
        return tab->rect;
    };
    // The other window stands to the right of this one on the desktop, its
    // units twice its points.
    h.ui().set_root_place(KitHarness::WINDOW, ui::core::RootPlace{.at = {100.0f, 50.0f}, .units_per_point = 1.0f});
    h.ui().set_root_place(other, ui::core::RootPlace{.at = {100.0f + h.size().x + 40.0f, 50.0f}, .units_per_point = 2.0f});
    const auto from_other = [&](glm::vec2 at) {
        return glm::vec2{h.size().x + 40.0f, 0.0f} + at / 2.0f;
    };

    // Carried off its row and over the other row, a little into the second
    // tab there: that row parts before it by the carried tab's width.
    const ui::core::Rect bark = tab_rect("bark.fjmat");
    const ui::core::Rect grass = tab_rect("grass.fjmat");
    const ui::core::Rect stone = tab_rect("stone.fjmesh");
    h.move_to(bark.centre());
    h.button_down();
    h.move_to({bark.centre().x, bark.centre().y + 200.0f});
    h.move_to(from_other({stone.min.x + 4.0f, stone.centre().y}));
    h.step(2);
    CHECK(tab_rect("grass.fjmat").min.x == Approx(grass.min.x));
    CHECK(tab_rect("stone.fjmesh").min.x > stone.min.x + bark.width() * 0.9f);
    // Its copy goes on past this window's edge, in a window of its own that
    // says nothing of opening one: the tab lands on the row.
    REQUIRE(h.windows().size() == 2);
    const ui::core::RootRequest& copy = h.windows()[1];
    CHECK(copy.kind == ui::core::RootKind::Tooltip);
    CHECK(copy.size.x < bark.width() + 60.0f);

    // Let go there, as the platform tells it: the button comes up in this
    // window, a move follows held to its edge as the platform lets go of the
    // mouse, then the mouse leaves it and comes into the other, all before
    // the frame. It lands on that row, told by the row's owner, and is shown
    // where the row parted once the program draws it there.
    using Kind = ui::core::PointerEvent::Kind;
    const glm::vec2 let_go = from_other({stone.min.x + 4.0f, stone.centre().y});
    h.send({.kind = Kind::Up, .sample = {.position = let_go}});
    h.send({.kind = Kind::Move, .sample = {.position = {h.size().x - 1.0f, let_go.y}}});
    h.send({.kind = Kind::Leave});
    h.send({.kind = Kind::Move, .root = other, .sample = {.position = {stone.min.x + 4.0f, stone.centre().y}}});
    h.step();
    REQUIRE(landed == 7);
    CHECK_FALSE(torn.has_value());
    here.pop_back();
    there.push_back("bark.fjmat");
    h.step(2);
    CHECK(places_there == std::vector<std::size_t>{0, 2, 1});
    CHECK(tab_rect("bark.fjmat").min.x < tab_rect("stone.fjmesh").min.x);
    CHECK(tab_rect("grass.fjmat").min.x < tab_rect("bark.fjmat").min.x);
    CHECK(tab_rect("stone.fjmesh").min.x == Approx(stone.min.x + bark.width() + ui::item_inner_spacing().x).margin(1.0f));

    // Where the platform keeps windows' places to itself, the row isn't
    // found, and let go there the tab opens in a window of its own.
    landed.reset();
    h.ui().set_root_place(KitHarness::WINDOW, std::nullopt);
    const ui::core::Rect grass_now = tab_rect("grass.fjmat");
    h.send(ui::core::PointerEvent{.kind = ui::core::PointerEvent::Kind::Move, .root = other, .sample = {.position = grass_now.centre()}});
    h.step();
    h.send(ui::core::PointerEvent{.kind = ui::core::PointerEvent::Kind::Down, .root = other, .sample = {.position = grass_now.centre()}});
    h.step();
    h.send(ui::core::PointerEvent{.kind = ui::core::PointerEvent::Kind::Move, .root = other,
                                  .sample = {.position = {grass_now.centre().x, grass_now.centre().y + 300.0f}}});
    h.step();
    h.send(ui::core::PointerEvent{.kind = ui::core::PointerEvent::Kind::Move, .root = other, .sample = {.position = {-400.0f, 20.0f}}});
    h.step(2);
    h.send(ui::core::PointerEvent{.kind = ui::core::PointerEvent::Kind::Up, .root = other, .sample = {.position = {-400.0f, 20.0f}}});
    h.step(2);
    CHECK_FALSE(landed.has_value());
    REQUIRE(torn.has_value());
    CHECK(torn->x < 0.0f);
}

TEST_CASE("A panel that lands on rows, carried over another window's row that takes panels, lands there",
          "[ui][kit][tabs][dock][other_window]") {
    KitHarness h;
    h.ui().set_popups_leave_window(true);
    bool built = false;
    std::optional<ui::LandedPanel> landed;
    h.set_ui([&] {
        const ui::Id dock = ui::id("##dock");
        ui::dockspace(dock);
        if (!built) {
            using N = ui::DockNode;
            ui::build_dock_tree(dock, {800.0f, 600.0f}, N::left_right(0.5f, N::tabs({"Viewport"}), N::tabs({"Notes"})));
            built = true;
        }
        if (auto panel = ui::Panel("", "Viewport")) ui::text("Viewport");
        // As a program does: the panel that landed is a tab there now.
        if (!landed) {
            if (auto panel = ui::Panel("", "Notes", {.lands_on_rows = true})) ui::text("Notes");
        }
        const ui::OtherWindow other(OTHER, {.title = "other", .size = {500.0f, 300.0f}});
        if (other) {
            if (auto docs = ui::DocumentTabs("##docs 7", {.group = "editor", .owner = 7, .takes_panels = true})) {
                if (docs.landed()) landed = docs.landed();
                (void)ui::document_tab({.label = "grass.fjmat"});
                (void)ui::document_tab({.label = "stone.fjmesh"});
            }
        }
    });
    h.step(3);
    const ui::core::RootId other = ui::other_window_root(OTHER);
    const auto tab_rect = [&](const char* name) {
        const ui::core::SemanticNode* tab = h.find(name, ui::core::Role::Tab);
        REQUIRE(tab != nullptr);
        return tab->rect;
    };
    // The other window stands to the right of this one, its units twice its
    // points.
    h.ui().set_root_place(KitHarness::WINDOW, ui::core::RootPlace{.at = {100.0f, 50.0f}, .units_per_point = 1.0f});
    h.ui().set_root_place(other, ui::core::RootPlace{.at = {100.0f + h.size().x + 40.0f, 50.0f}, .units_per_point = 2.0f});
    const auto from_other = [&](glm::vec2 at) { return glm::vec2{h.size().x + 40.0f, 0.0f} + at / 2.0f; };

    // Torn out of its part by its tab and carried over the other window's
    // row, a little into its second tab: the row parts there.
    const ui::core::Rect notes = tab_rect("Notes");
    const ui::core::Rect grass = tab_rect("grass.fjmat");
    const ui::core::Rect stone = tab_rect("stone.fjmesh");
    h.move_to(notes.centre());
    h.button_down();
    h.move_to({notes.centre().x, notes.centre().y + 120.0f});
    h.move_to(from_other({stone.min.x + 4.0f, stone.centre().y}));
    h.step(2);
    CHECK(tab_rect("grass.fjmat").min.x == Approx(grass.min.x));
    CHECK(tab_rect("stone.fjmesh").min.x > stone.min.x + 20.0f);
    // Its copy goes on past this window's edge, saying nothing of opening a
    // window of its own.
    const auto copy = std::ranges::find(h.windows(), ui::core::RootKind::Tooltip, &ui::core::RootRequest::kind);
    REQUIRE(copy != h.windows().end());
    CHECK(copy->size.x < notes.width() + 60.0f);

    // Let go there, as the platform tells it.
    using Kind = ui::core::PointerEvent::Kind;
    const glm::vec2 let_go = from_other({stone.min.x + 4.0f, stone.centre().y});
    h.send({.kind = Kind::Up, .sample = {.position = let_go}});
    h.send({.kind = Kind::Move, .sample = {.position = {h.size().x - 1.0f, let_go.y}}});
    h.send({.kind = Kind::Leave});
    h.send({.kind = Kind::Move, .root = other, .sample = {.position = {stone.min.x + 4.0f, stone.centre().y}}});
    h.step(3);
    REQUIRE(landed.has_value());
    CHECK(*landed == ui::LandedPanel{.window = "Notes", .place = 1});
    CHECK_FALSE(ui::torn_off_window("Notes").has_value());
}

TEST_CASE("A tab that docks, carried over another window's dockspace, shows its targets there and says where",
          "[ui][kit][tabs][dock][other_window]") {
    using N = ui::DockNode;
    KitHarness h;
    h.ui().set_popups_leave_window(true);
    bool home_built = false;
    bool there_built = false;
    ui::Id home = 0;
    ui::Id there = 0;
    std::optional<ui::DockPlace> docked;
    h.set_ui([&] {
        // This window: a row with a Code tab that docks, over the dockspace
        // its page is in.
        home = ui::id("##home");
        if (auto docs = ui::DocumentTabs("##docs")) {
            (void)ui::document_tab({.label = "Scene", .closable = false});
            const ui::DocumentTabResult code = ui::document_tab(
                {.label = "Code", .closable = false, .docks = true, .dock_share = 0.4f, .dockspace = home});
            if (code.docked) docked = code.docked;
        }
        ui::dockspace(home);
        if (!home_built) {
            ui::build_dock_tree(home, {800.0f, 560.0f}, N::tabs({"Page"}));
            home_built = true;
        }
        if (auto panel = ui::Panel("", "Page")) ui::text("Page");
        // The other: the Hierarchy beside the Viewport.
        const ui::OtherWindow other(OTHER, {.title = "other", .size = {600.0f, 400.0f}});
        if (!other) return;
        there = ui::id("##there");
        ui::next_window_place({0.0f, 0.0f}, {600.0f, 400.0f});
        (void)ui::begin_host_window("##there_host",
                                    {.background = false, .docking = false, .to_front = false, .nav_focus = false});
        ui::dockspace(there);
        if (!there_built) {
            ui::build_dock_tree(there, {600.0f, 400.0f}, N::left_right(0.3f, N::tabs({"Hierarchy"}), N::tabs({"Viewport"})));
            there_built = true;
        }
        ui::end_host_window();
        for (const char* name : {"Hierarchy", "Viewport"}) {
            if (auto panel = ui::Panel("", name)) ui::text(name);
        }
    });
    h.step(4);
    const ui::core::RootId other = ui::other_window_root(OTHER);
    REQUIRE(ui::window_dock("Viewport") != 0);
    h.ui().set_root_place(KitHarness::WINDOW, ui::core::RootPlace{.at = {100.0f, 50.0f}, .units_per_point = 1.0f});
    h.ui().set_root_place(other, ui::core::RootPlace{.at = {100.0f + h.size().x + 40.0f, 50.0f}, .units_per_point = 2.0f});
    const auto from_other = [&](glm::vec2 at) { return glm::vec2{h.size().x + 40.0f, 0.0f} + at / 2.0f; };
    const ui::core::SemanticNode* code_tab = h.find("Code", ui::core::Role::Tab);
    REQUIRE(code_tab != nullptr);
    const glm::vec2 code = code_tab->rect.centre();
    const auto viewport = ui::window_named("Viewport");
    REQUIRE(viewport.has_value());
    // The right of the Viewport's targets, a target's step from its middle.
    const glm::vec2 right = viewport->pos + viewport->size * 0.5f + glm::vec2{38.0f, 0.0f};

    // Over its own page it docks nowhere; over the other window's Viewport,
    // let go as the platform tells it, beside it there.
    h.move_to(code);
    h.button_down();
    h.move_to({code.x, code.y + 200.0f});
    h.step(2);
    h.move_to(from_other(right));
    h.step(2);
    using Kind = ui::core::PointerEvent::Kind;
    h.send({.kind = Kind::Up, .sample = {.position = from_other(right)}});
    h.send({.kind = Kind::Move, .sample = {.position = {h.size().x - 1.0f, from_other(right).y}}});
    h.send({.kind = Kind::Leave});
    h.send({.kind = Kind::Move, .root = other, .sample = {.position = right}});
    h.step(3);
    REQUIRE(docked.has_value());
    CHECK(docked->node == ui::window_dock("Viewport"));
    CHECK(docked->side == ui::DockSide::Right);
    CHECK(ui::dockspace_of(docked->node) == there);

    // Let go on its own page, it docks nowhere.
    docked.reset();
    h.send({.kind = Kind::Leave, .root = other});
    h.move_to(code);
    h.button_down();
    h.move_to({code.x, code.y + 200.0f});
    h.move_to({code.x + 100.0f, code.y + 260.0f});
    h.button_up();
    h.step(2);
    CHECK_FALSE(docked.has_value());
}
