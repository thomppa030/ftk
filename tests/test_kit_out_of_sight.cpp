#include "kit_harness.hpp"

#include "ftk/ui/kit/field.hpp"
#include "ftk/ui/kit/item.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/panel.hpp"
#include "ftk/ui/kit/row.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/tree.hpp"
#include "ftk/ui/kit/window.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// What scrolls out of a panel's sight costs only its room: the rows of a
// long table and of a long tree are skipped, keeping their height, unless
// they hold what a pointer or the keyboard has.

using Catch::Approx;
using ftk::test::KitHarness;
namespace ui = ftk::ui;

namespace {

constexpr int ROWS = 200;
constexpr glm::vec2 PANEL{300.0f, 200.0f};

// A panel of ROWS rows of a number each, scrolled to `scroll` when asked.
struct Table {
    KitHarness h;
    std::vector<float> values = std::vector<float>(ROWS, 0.5f);
    std::vector<std::string> labels;
    // The rows whose value was drawn last frame, and how far the panel
    // scrolls.
    std::vector<int> drawn;
    float max_scroll = 0.0f;
    std::optional<float> scroll_to;

    Table() {
        for (int i = 0; i < ROWS; ++i) labels.push_back("Value " + std::to_string(i));
        h.set_ui([this] {
            drawn.clear();
            ui::next_window_place({0.0f, 0.0f}, PANEL);
            if (auto panel = ui::Panel("", "Values")) {
                if (scroll_to) ui::set_scroll_y(*std::exchange(scroll_to, std::nullopt));
                if (auto table = ui::PropertyTable("##values")) {
                    for (int i = 0; i < ROWS; ++i) {
                        ui::IdScope scope(i);
                        ui::row(labels[static_cast<std::size_t>(i)].c_str(), [&] {
                            (void)ui::drag("##value", values[static_cast<std::size_t>(i)]);
                            if (i == 0) h.mark("first");
                            drawn.push_back(i);
                        });
                    }
                }
                max_scroll = ui::scroll_max_y();
            }
        });
    }
};

} // namespace

TEST_CASE("A long table draws the rows in sight and keeps the room of the rest", "[ui][kit]") {
    Table t;
    // The first frame draws every row, to learn how tall each is.
    t.h.step();
    CHECK(t.drawn.size() == ROWS);
    t.h.step();
    const float all_drawn = t.max_scroll;
    REQUIRE(all_drawn > 0.0f);

    t.h.step(2);
    CHECK(t.drawn.size() < 20);
    CHECK(t.drawn.front() == 0);
    // Skipped, the rows keep the panel as tall as when they were drawn.
    CHECK(t.max_scroll == Approx(all_drawn));

    // Scrolled to the end, the last rows are the ones drawn.
    t.scroll_to = all_drawn;
    t.h.step(2);
    CHECK(t.drawn.size() < 20);
    CHECK(t.drawn.back() == ROWS - 1);
    CHECK(t.max_scroll == Approx(all_drawn));
}

TEST_CASE("A number held as its row scrolls out of sight goes on being dragged", "[ui][kit]") {
    Table t;
    t.h.step(3);
    const glm::vec2 at = t.h.rect("first").centre();
    t.h.move_to(at);
    t.h.button_down();
    t.h.move_to(at + glm::vec2{10.0f, 0.0f});

    // The panel scrolls the held row far out of sight.
    t.scroll_to = 1000.0f;
    t.h.step(2);
    CHECK(std::ranges::find(t.drawn, 0) != t.drawn.end());
    const float before = t.values[0];
    t.h.move_to(at + glm::vec2{60.0f, 0.0f});
    CHECK(t.values[0] > before);

    // Let go, it is skipped again.
    t.h.button_up();
    t.h.step(2);
    CHECK(std::ranges::find(t.drawn, 0) == t.drawn.end());
}

TEST_CASE("A long tree draws the rows in sight and keeps their room and their folds", "[ui][kit]") {
    KitHarness h;
    std::vector<std::string> names;
    for (int i = 0; i < ROWS; ++i) names.push_back("Crate " + std::to_string(i));
    float max_scroll = 0.0f;
    std::optional<float> scroll_to;
    bool open = false;
    h.set_ui([&] {
        ui::next_window_place({0.0f, 0.0f}, PANEL);
        if (auto panel = ui::Panel("", "Scene")) {
            if (scroll_to) ui::set_scroll_y(*std::exchange(scroll_to, std::nullopt));
            if (auto tree = ui::Tree("##scene")) {
                open = ui::tree_row({.id = "##root", .name = "Harbour", .has_children = true}).open;
                if (open) {
                    for (const std::string& name : names) {
                        (void)ui::tree_row({.id = name.c_str(), .name = name, .depth = 1});
                    }
                }
            }
            max_scroll = ui::scroll_max_y();
        }
    });
    const auto rows = [&] {
        const auto nodes = h.ui().semantics();
        return std::ranges::count(nodes, ui::core::Role::TreeItem, &ui::core::SemanticNode::role);
    };
    h.step(2);
    const float all = max_scroll;
    REQUIRE(all > 0.0f);
    CHECK(rows() < 20);
    CHECK(h.find("Crate 0") != nullptr);
    CHECK(h.find("Crate 199") == nullptr);

    // At the end, the root out of sight stays open and the last rows show.
    scroll_to = all;
    h.step(2);
    CHECK(open);
    CHECK(rows() < 20);
    CHECK(h.find("Harbour") == nullptr);
    CHECK(h.find("Crate 199") != nullptr);
    CHECK(max_scroll == Approx(all));
}
