#include "kit_harness.hpp"

#include "ftk/ui/kit/dock.hpp"
#include "ftk/ui/kit/panel.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/tabs.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/window.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <optional>

// A document tab that docks: carried off its row over a dockspace, the
// dock's targets show and letting go says where it docks; carried back over
// its row, the tab held under it comes forward.

using Catch::Approx;
using ftk::test::KitHarness;
namespace ui = ftk::ui;

namespace {

constexpr float TARGET_STEP = 38.0f;  // from a part's middle target to a side's

// A row with the Scene and a Code tab that docks, over a dockspace with the
// Hierarchy beside the Viewport.
struct Row {
    KitHarness h;
    ui::Id dock{0};
    ui::DocumentTabResult scene{};
    ui::DocumentTabResult code{};
    std::optional<ui::DockPlace> docked;

    Row() {
        bool built = false;
        h.set_ui([this, built]() mutable {
            if (auto tabs = ui::DocumentTabs("##docs")) {
                scene = ui::document_tab({.label = "Scene", .closable = false});
                code = ui::document_tab({.label = "Code", .closable = false, .docks = true, .dock_share = 0.4f});
                if (code.docked) docked = code.docked;
            }
            dock = ui::id("##dock");
            ui::dockspace(dock);
            if (!built) {
                using N = ui::DockNode;
                ui::build_dock_tree(dock, {800.0f, 560.0f},
                                    N::left_right(0.3f, N::tabs({"Hierarchy"}), N::tabs({"Viewport"})));
                built = true;
            }
            for (const char* name : {"Hierarchy", "Viewport"}) {
                if (auto panel = ui::Panel("", name)) ui::text(name);
            }
        });
        h.step(4);
    }

    [[nodiscard]] glm::vec2 tab(const char* name) const {
        const ui::core::SemanticNode* found = h.find(name, ui::core::Role::Tab);
        REQUIRE(found != nullptr);
        return found->rect.centre();
    }

    [[nodiscard]] static glm::vec2 middle_of(const char* window) {
        const auto shown = ui::window_named(window);
        REQUIRE(shown.has_value());
        return shown->pos + shown->size * 0.5f;
    }

    // Carries the Code tab off its row to `to`, and lets it go there.
    void carry_code_to(glm::vec2 to) {
        const glm::vec2 from = tab("Code");
        h.move_to(from);
        h.button_down();
        h.move_to({from.x, from.y + 200.0f});
        h.move_to(to);
        h.button_up();
        h.step(2);
    }
};

} // namespace

TEST_CASE("A tab that docks, carried off its row, shows the dock's targets and says where it was let go",
          "[ui][kit][tabs][dock]") {
    Row r;
    const glm::vec2 viewport = Row::middle_of("Viewport");

    // Lifted, it says so while it is carried.
    const glm::vec2 from = r.tab("Code");
    r.h.move_to(from);
    r.h.button_down();
    r.h.move_to({from.x, from.y + 200.0f});
    CHECK(r.code.lifted);
    // Let go on the right of the Viewport's targets: beside it, its share.
    r.h.move_to(viewport + glm::vec2{TARGET_STEP, 0.0f});
    r.h.button_up();
    r.h.step(2);
    CHECK_FALSE(r.code.lifted);
    REQUIRE(r.docked.has_value());
    CHECK(r.docked->node == ui::window_dock("Viewport"));
    CHECK(r.docked->side == ui::DockSide::Right);
    CHECK(r.docked->share == Approx(0.4f).margin(0.01f));

    // On its middle: among its tabs.
    r.docked.reset();
    r.carry_code_to(viewport);
    REQUIRE(r.docked.has_value());
    CHECK(r.docked->node == ui::window_dock("Viewport"));
    CHECK_FALSE(r.docked->side.has_value());

    // Off every target: nowhere, and the row as it was.
    r.docked.reset();
    const auto hierarchy = ui::window_named("Hierarchy");
    REQUIRE(hierarchy.has_value());
    r.carry_code_to(hierarchy->pos + glm::vec2{20.0f, 60.0f});
    CHECK_FALSE(r.docked.has_value());
    CHECK(r.tab("Scene").x < r.tab("Code").x);
}

TEST_CASE("A tab that docks, carried back over its row and held on another tab, brings that one forward",
          "[ui][kit][tabs][dock]") {
    Row r;
    const glm::vec2 code = r.tab("Code");
    const glm::vec2 scene = r.tab("Scene");
    r.h.move_to(code);
    r.h.button_down();
    REQUIRE(r.code.front);
    r.h.move_to({code.x, code.y + 200.0f});
    // Back over its row, it stays carried: no tab takes another's place.
    r.h.move_to(scene);
    r.h.step(6);
    CHECK(r.code.lifted);
    CHECK_FALSE(r.scene.front);
    // Held long enough, the tab under it comes forward.
    r.h.step(40);
    CHECK(r.scene.front);
    CHECK_FALSE(r.code.front);
    // Let go there, it stays where it was.
    r.h.button_up();
    r.h.step(2);
    CHECK_FALSE(r.docked.has_value());
    CHECK(r.tab("Scene").x < r.tab("Code").x);
    CHECK(r.scene.front);
}
