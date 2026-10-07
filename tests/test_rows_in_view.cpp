#include "kit_harness.hpp"

#include "ftk/ui/kit/field.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/region.hpp"
#include "ftk/ui/kit/rows_in_view.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <span>
#include <string>
#include <vector>

using ftk::test::KitHarness;
namespace ui = ftk::ui;

namespace {

// Rows of three heights in turn, as a log's messages of one, two and three
// lines are.
float height_of(std::size_t i) {
    return 20.0f + static_cast<float>(i % 3) * 16.0f;
}

// `count` such rows in a region 300 tall, drawn through a RowsInView of
// their tops, or with `walk` every one of them: where the walked rows sit
// is where the counted ones must.
struct VariedRows {
    KitHarness h;
    std::size_t count;
    bool walked;
    std::vector<float> tops;
    // Each row drawn on the last frame, by its top below the run's top.
    std::map<std::size_t, float> drawn;
    // Where what follows the run begins, below the run's top.
    float after{0.0f};
    glm::vec2 centre{};

    VariedRows(std::size_t rows, bool walk) : count{rows}, walked{walk} {
        h.set_ui([this] {
            if (auto region = ui::Region("##rows", {300.0f, 300.0f})) {
                drawn.clear();
                const float top = ui::cursor().y;
                const float spacing = ui::item_spacing().y;
                const auto row = [&](std::size_t i) {
                    ui::reserve({100.0f, height_of(i)});
                    drawn[i] = h.ui().layout().last_item().min.y - top;
                };
                if (walked) {
                    for (std::size_t i = 0; i < count; ++i) row(i);
                } else {
                    tops.assign(1, 0.0f);
                    for (std::size_t i = 0; i < count; ++i) tops.push_back(tops.back() + height_of(i) + spacing);
                    ui::RowsInView run{std::span<const float>(tops)};
                    for (const std::size_t i : run.rows()) {
                        run.at(i);
                        row(i);
                    }
                    run.finish();
                }
                after = ui::cursor().y - top;
            }
            h.mark("region");
        });
        h.step(3);
        centre = h.rect("region").centre();
    }

    // Turns the wheel over the region and lets the scroll settle.
    void scroll(float notches) {
        h.wheel(centre, notches);
        h.step(60);
    }
};

// Where a run of 3000 varied rows draws its rows and ends: at the top, part
// way down, and scrolled to the end.
std::vector<std::map<std::size_t, float>> varied_places(bool walked, float& after) {
    VariedRows rows(3000, walked);
    std::vector<std::map<std::size_t, float>> places{rows.drawn};
    rows.scroll(-50.0f);
    places.push_back(rows.drawn);
    rows.scroll(-100000.0f);
    places.push_back(rows.drawn);
    after = rows.after;
    return places;
}

} // namespace

TEST_CASE("Rows of known heights in view are drawn where drawing every row puts them", "[ui][kit]") {
    float walked_after = 0.0f;
    float counted_after = 0.0f;
    const auto walked = varied_places(true, walked_after);
    const auto counted = varied_places(false, counted_after);
    // Scrolled part way, then to the last row.
    REQUIRE(!counted[1].empty());
    CHECK(counted[1].begin()->first > 20);
    CHECK(counted[2].contains(2999));
    // A screenful, not the run.
    CHECK(counted[1].size() < 30);
    CHECK(std::abs(counted_after - walked_after) < 0.5f);
    for (std::size_t place = 0; place < walked.size(); ++place) {
        INFO("place " << place);
        for (const auto& [row, y] : counted[place]) {
            INFO("row " << row);
            REQUIRE(walked[place].contains(row));
            CHECK(std::abs(walked[place].at(row) - y) < 0.5f);
        }
    }
}

TEST_CASE("A row holding the pointer is drawn while it is scrolled out of sight", "[ui][kit]") {
    KitHarness h;
    std::vector<float> values(1000, 0.0f);
    std::vector<std::size_t> visited;
    h.set_ui([&] {
        if (auto region = ui::Region("##rows", {300.0f, 200.0f})) {
            ui::RowsInView run(values.size(), {.pitch = ui::frame_height() + ui::item_spacing().y});
            visited = run.rows();
            for (const std::size_t i : run.rows()) {
                run.at(i);
                const std::string id = "##value" + std::to_string(i);
                (void)ui::drag(id.c_str(), values[i]);
                h.mark("value" + std::to_string(i));
            }
        }
    });
    h.step(3);
    const glm::vec2 field = h.rect("value2").centre();
    h.move_to(field);
    h.button_down();
    h.wheel(field, -30.0f);
    h.step(60);
    // The held row first, above those in sight far down the run.
    REQUIRE(visited.size() > 1);
    CHECK(visited.front() == 2);
    CHECK(visited[1] > 10);
    // Its number goes on being dragged.
    h.move_to(field + glm::vec2{40.0f, 0.0f});
    CHECK(values[2] != 0.0f);

    // Let go, it is out of sight like the rest.
    h.button_up();
    h.step(3);
    CHECK(std::ranges::find(visited, std::size_t{2}) == visited.end());
}
