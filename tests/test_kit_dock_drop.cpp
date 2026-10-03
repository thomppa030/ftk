#include "kit_harness.hpp"

#include "ftk/ui/kit/dock.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/panel.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/window.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

// Where a window carried over a dockspace docks: among a leaf's tabs at its
// middle target, beside it at its side targets, or beside the whole
// dockspace at its edges' targets.

using Catch::Approx;
using ftk::test::KitHarness;
namespace ui = ftk::ui;

namespace {

constexpr float TARGET_STEP = 38.0f;  // a target's side and the gap between

// A dockspace laid out as `layout` (none: left empty), and a window floating
// over it, "Notes".
struct Dropping {
    KitHarness h;
    ui::Id dock{0};
    std::vector<std::string> windows;

    Dropping(std::vector<std::string> docked, std::optional<ui::DockNode> layout) : windows(std::move(docked)) {
        windows.push_back("Notes");
        bool built = false;
        h.set_ui([this, layout, built]() mutable {
            dock = ui::id("##dock");
            ui::dockspace(dock);
            if (!built) {
                if (layout) ui::build_dock_tree(dock, {800.0f, 600.0f}, *layout);
                built = true;
            }
            for (const std::string& name : windows) {
                if (name == "Notes") ui::next_window_size_once({150.0f, 120.0f});
                if (auto panel = ui::Panel("", name.c_str())) ui::text(name);
            }
        });
        h.step(4);
    }

    // Carries Notes by its title bar to `to`, and lets it go there.
    void drop_at(glm::vec2 to) {
        const auto notes = ui::window_named("Notes");
        REQUIRE(notes.has_value());
        h.drag({notes->pos.x + 40.0f, notes->pos.y + ui::frame_height() * 0.5f}, to);
        h.step(2);
    }

    [[nodiscard]] glm::vec2 middle_of(const char* window) const {
        const auto shown = ui::window_named(window);
        REQUIRE(shown.has_value());
        return shown->pos + shown->size * 0.5f;
    }

    [[nodiscard]] ui::DockNode tree() const {
        const auto read = ui::dock_tree(dock, windows);
        REQUIRE(read.has_value());
        return *read;
    }
};

} // namespace

TEST_CASE("A window dropped on a leaf's middle target joins its tabs", "[ui][kit][dock]") {
    using N = ui::DockNode;
    Dropping d({"Hierarchy", "Viewport"}, N::left_right(0.5f, N::tabs({"Hierarchy"}), N::tabs({"Viewport"})));
    d.drop_at(d.middle_of("Viewport"));
    CHECK(ui::window_dock("Notes") != 0);
    CHECK(ui::window_dock("Notes") == ui::window_dock("Viewport"));
}

TEST_CASE("A window dropped on a leaf's side target splits the leaf, its part its own size", "[ui][kit][dock]") {
    using N = ui::DockNode;
    Dropping d({"Hierarchy", "Viewport"}, N::left_right(0.5f, N::tabs({"Hierarchy"}), N::tabs({"Viewport"})));
    const auto viewport = ui::window_named("Viewport");
    d.drop_at(d.middle_of("Viewport") + glm::vec2{TARGET_STEP, 0.0f});
    const ui::DockNode tree = d.tree();
    REQUIRE(tree.children.size() == 2);
    CHECK(tree.children[0].windows == std::vector<std::string>{"Hierarchy"});
    const ui::DockNode& right = tree.children[1];
    REQUIRE(right.split == ui::DockAxis::LeftRight);
    CHECK(right.children[0].windows == std::vector<std::string>{"Viewport"});
    CHECK(right.children[1].windows == std::vector<std::string>{"Notes"});
    // Notes takes its own width of the leaf, at most half of it.
    CHECK(right.ratio == Approx(1.0f - 150.0f / viewport->size.x).margin(0.01f));
}

TEST_CASE("A window dropped on a dockspace's edge target splits the whole of it", "[ui][kit][dock]") {
    using N = ui::DockNode;
    Dropping d({"Hierarchy", "Viewport"}, N::left_right(0.5f, N::tabs({"Hierarchy"}), N::tabs({"Viewport"})));
    const auto left = ui::window_named("Hierarchy");
    const auto right = ui::window_named("Viewport");
    // The middle of the dockspace's top edge, a little in.
    const float middle = (left->pos.x + right->pos.x + right->size.x) * 0.5f;
    d.drop_at({middle, left->pos.y + 22.0f});
    const ui::DockNode tree = d.tree();
    REQUIRE(tree.split == ui::DockAxis::TopBottom);
    REQUIRE(tree.children.size() == 2);
    CHECK(tree.children[0].windows == std::vector<std::string>{"Notes"});
    const ui::DockNode& below = tree.children[1];
    CHECK(below.split == ui::DockAxis::LeftRight);
    CHECK(below.ratio == Approx(0.5f).margin(0.01f));
    REQUIRE(below.children.size() == 2);
    CHECK(below.children[0].windows == std::vector<std::string>{"Hierarchy"});
    CHECK(below.children[1].windows == std::vector<std::string>{"Viewport"});
}

TEST_CASE("A window torn out of the middle leaves no hole: what is left grows into its room", "[ui][kit][dock]") {
    using N = ui::DockNode;
    // The Viewport is the dockspace's middle: what the split leaves of it.
    Dropping d({"Hierarchy", "Viewport"}, N::left_right(0.5f, N::tabs({"Hierarchy"}), N::tabs({"Viewport"})));
    const auto viewport = ui::window_named("Viewport");
    REQUIRE(viewport.has_value());
    const float bar = ui::frame_height() * 0.5f;
    d.h.drag({viewport->pos.x + 45.0f, viewport->pos.y + bar},
             {viewport->pos.x + 60.0f, viewport->pos.y + viewport->size.y - 60.0f});
    d.h.step(2);
    CHECK(ui::window_dock("Viewport") == 0);
    const ui::DockNode tree = d.tree();
    CHECK(tree.split == ui::DockAxis::None);
    CHECK(tree.windows == std::vector<std::string>{"Hierarchy"});
    CHECK(ui::window_named("Hierarchy")->size.x > viewport->size.x * 1.8f);
}

TEST_CASE("A part whose windows aren't drawn lends its room, and takes it back when they are", "[ui][kit][dock]") {
    using N = ui::DockNode;
    KitHarness h;
    bool built = false;
    bool hierarchy = true;
    h.set_ui([&] {
        const ui::Id dock = ui::id("##dock");
        ui::dockspace(dock);
        if (!built) {
            ui::build_dock_tree(dock, {800.0f, 600.0f}, N::left_right(0.3f, N::tabs({"Hierarchy"}), N::tabs({"Viewport"})));
            built = true;
        }
        if (hierarchy) {
            if (auto panel = ui::Panel("", "Hierarchy")) ui::text("Hierarchy");
        }
        if (auto panel = ui::Panel("", "Viewport")) ui::text("Viewport");
    });
    h.step(4);
    const float shared = ui::window_named("Viewport")->size.x;

    // Closed for now: the Viewport takes the whole dockspace.
    hierarchy = false;
    h.step(3);
    CHECK(ui::window_named("Viewport")->size.x > shared + 200.0f);
    CHECK(ui::window_dock("Hierarchy") != 0);

    // Drawn again, it is back in its place.
    hierarchy = true;
    h.step(3);
    CHECK(ui::window_named("Viewport")->size.x == Approx(shared).margin(1.0f));
    CHECK(ui::window_named("Hierarchy")->size.x > 100.0f);
}

TEST_CASE("An empty dockspace takes a window dropped in its middle", "[ui][kit][dock]") {
    Dropping d({}, std::nullopt);
    const auto notes = ui::window_named("Notes");
    REQUIRE(notes.has_value());
    // The dockspace fills the harness's window.
    d.drop_at(d.h.size() * 0.5f);
    CHECK(ui::window_dock("Notes") != 0);
    const ui::DockNode tree = d.tree();
    CHECK(tree.split == ui::DockAxis::None);
    CHECK(tree.windows == std::vector<std::string>{"Notes"});
}
