#include "kit_harness.hpp"

#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/pick_row.hpp"
#include "ftk/ui/kit/region.hpp"
#include "ftk/ui/kit/rows_in_view.hpp"
#include "ftk/ui/theme.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

using ftk::test::KitHarness;
namespace ui = ftk::ui;
namespace core = ftk::ui::core;

namespace {

// Whether the last frame drew anything in `colour` inside `rect`.
bool drew_in(const KitHarness& h, core::Rect rect, ui::Color colour) {
    const uint32_t packed = core::pack_colour(colour);
    for (const auto& root : h.output().roots) {
        for (const auto& p : root.draw->primitives) {
            if (p.colour == packed && rect.contains((p.min + p.max) * 0.5f)) return true;
        }
    }
    return false;
}

// One row under test, ticked as its box says.
struct Row {
    KitHarness h;
    ui::PickRowSpec spec{.id = "##crate", .picked = true, .icon = "M", .hue = ftk::theme::Hue::blue,
                         .name = "crate.gltf", .figure = "264"};
    bool selected{false};
    int clicks{0};
    int toggles{0};
    int opens{0};

    Row() {
        h.set_ui([this] {
            const auto result = ui::pick_row(spec, selected);
            h.mark("row");
            if (result.clicked) ++clicks;
            if (result.toggled) {
                ++toggles;
                spec.picked = !spec.picked;
            }
            if (result.double_clicked) ++opens;
        });
        h.step(2);
    }

    [[nodiscard]] core::Rect box() { return h.ui().find("crate.gltf", core::Role::Toggle)->rect; }
};

} // namespace

TEST_CASE("A pick row tells a click on its box from a click on the row", "[ui][kit]") {
    Row r;
    REQUIRE(r.h.ui().find("crate.gltf", core::Role::Toggle) != nullptr);
    CHECK(r.h.ui().find("crate.gltf", core::Role::Toggle)->state.checked);

    r.h.click_at(r.box().centre());
    CHECK(r.toggles == 1);
    CHECK(r.clicks == 0);
    CHECK_FALSE(r.spec.picked);
    r.h.step();
    CHECK_FALSE(r.h.ui().find("crate.gltf", core::Role::Toggle)->state.checked);

    // On the name: the row is clicked, its tick stays.
    const core::Rect row = r.h.rect("row");
    r.h.click_at({row.min.x + (row.width() * 0.5f), row.centre().y});
    CHECK(r.clicks == 1);
    CHECK(r.toggles == 1);
    CHECK_FALSE(r.spec.picked);

    // A double-click on the row opens; one on the box only ticks.
    r.h.double_click_at({row.min.x + (row.width() * 0.5f), row.centre().y});
    CHECK(r.opens == 1);
    const int opens = r.opens;
    r.h.double_click_at(r.box().centre());
    CHECK(r.opens == opens);
}

TEST_CASE("A locked pick row's box changes nothing and is no row click either", "[ui][kit]") {
    Row r;
    r.spec.locked = true;
    r.h.step();
    r.h.click_at(r.box().centre());
    CHECK(r.toggles == 0);
    CHECK(r.clicks == 0);
    CHECK(r.spec.picked);
    CHECK(r.h.ui().find("crate.gltf", core::Role::Toggle)->state.disabled);
    // The row itself can still be clicked, to show it.
    const core::Rect row = r.h.rect("row");
    r.h.click_at({row.max.x - 40.0f, row.centre().y});
    CHECK(r.clicks == 1);
}

TEST_CASE("A pick row shows its figure, or how its work stands", "[ui][kit]") {
    Row r;
    const core::Rect row = r.h.rect("row");
    const core::Rect right_end{.min = {row.max.x - 40.0f, row.min.y}, .max = row.max};
    const core::Rect foot{.min = {row.min.x, row.max.y - 2.0f}, .max = {row.max.x, row.max.y + 0.5f}};
    CHECK(drew_in(r.h, right_end, ftk::theme::text_disabled()));  // the figure
    CHECK(r.h.ui().find("crate.gltf", core::Role::ListItem)->value == "264");

    r.spec.work = ui::PickRowWork::done;
    r.h.step();
    CHECK(drew_in(r.h, right_end, ftk::theme::success()));
    CHECK_FALSE(drew_in(r.h, right_end, ftk::theme::text_disabled()));

    r.spec.work = ui::PickRowWork::failed;
    r.h.step();
    CHECK(drew_in(r.h, right_end, ftk::theme::error()));

    // Running: a bar along the foot, filled in the accent to its share.
    r.spec.work = ui::PickRowWork::running;
    r.spec.progress = 0.5f;
    r.h.step();
    CHECK(drew_in(r.h, foot, ftk::theme::accent()));
    CHECK(drew_in(r.h, foot, ftk::theme::surface_highest()));
    r.spec.progress = 0.0f;
    r.h.step();
    CHECK_FALSE(drew_in(r.h, foot, ftk::theme::accent()));
    // A share past the ends is held to them.
    r.spec.progress = 7.0f;
    r.h.step();
    CHECK(drew_in(r.h, foot, ftk::theme::accent()));

    // Waiting: the name faint, no bar. The box's tick keeps the text colour.
    const core::Rect name{.min = {row.min.x + 52.0f, row.min.y}, .max = {row.max.x - 40.0f, row.max.y}};
    CHECK(drew_in(r.h, name, ftk::theme::text()));
    r.spec.work = ui::PickRowWork::waiting;
    r.h.step();
    CHECK_FALSE(drew_in(r.h, foot, ftk::theme::accent()));
    CHECK_FALSE(drew_in(r.h, name, ftk::theme::text()));
    CHECK(drew_in(r.h, name, ftk::theme::text_disabled()));
}

TEST_CASE("A pick row is washed while selected and lit under the mouse", "[ui][kit]") {
    Row r;
    const core::Rect row = r.h.rect("row");
    CHECK_FALSE(drew_in(r.h, row, ftk::theme::selection()));
    r.selected = true;
    r.h.step();
    CHECK(drew_in(r.h, row, ftk::theme::selection()));
    CHECK(r.h.ui().find("crate.gltf", core::Role::ListItem)->state.selected);
    r.selected = false;
    r.h.move_to(row.centre());
    r.h.step(2);
    CHECK(drew_in(r.h, row, ftk::theme::surface_hover()));
}

TEST_CASE("A long name is cut and whole in the tooltip; an empty one still draws", "[ui][kit]") {
    Row r;
    r.spec.name = "Corner_ExteriorWide_Brick_with_a_name_much_longer_than_any_row_this_narrow_can_hold.gltf";
    r.h.set_ui([&r] {
        ui::next_width(160.0f);
        if (auto narrow = ui::Region("##narrow", {160.0f, 40.0f})) {
            (void)ui::pick_row(r.spec, false);
            r.h.mark("row");
        }
    });
    r.h.step(2);
    r.h.hover("row");
    r.h.step(20);
    CHECK(r.h.tooltip().has_value());

    r.spec.name = "";
    r.spec.figure = "";
    r.spec.icon = nullptr;
    r.h.step(2);
    CHECK(r.h.rect("row").height() == ftk::theme::TREE_ROW);
}

TEST_CASE("Of a long list of pick rows only those in sight are drawn", "[ui][kit]") {
    KitHarness h{{400.0f, 300.0f}};
    std::vector<std::string> names;
    for (int i = 0; i < 2000; ++i) names.push_back("Model_" + std::to_string(i) + ".gltf");
    h.set_ui([&] {
        if (auto list = ui::Region("##list", {300.0f, 200.0f})) {
            ui::RowsInView rows(names.size(), ui::RowPitch{.pitch = ftk::theme::TREE_ROW});
            for (const std::size_t i : rows.rows()) {
                rows.at(i);
                const std::string id = "##" + names[i];
                (void)ui::pick_row({.id = id.c_str(), .picked = true, .name = names[i]}, false);
            }
        }
    });
    h.step(2);
    const auto nodes = h.ui().semantics();
    const auto drawn = std::ranges::count(nodes, core::Role::ListItem, &core::SemanticNode::role);
    CHECK(drawn > 0);
    CHECK(drawn < 20);
}
