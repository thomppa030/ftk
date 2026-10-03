#include "kit_harness.hpp"

#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/rail.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>

using ftk::test::KitHarness;
namespace ui = ftk::ui;

namespace {

// Two steps with items between and after them, the first item shown at a
// second place too.
struct Fixture {
    KitHarness h;
    std::array<ui::RailEntry, 5> entries{ui::RailEntry{.label = "Load"},
                                         ui::RailEntry{.label = "Resize", .item = 0},
                                         ui::RailEntry{.label = "Save"},
                                         ui::RailEntry{.label = "Upload", .item = 1},
                                         ui::RailEntry{.label = "Resize", .item = 0, .note = "again"}};
    int clicked{-1};
    glm::vec2 origin{};

    Fixture() {
        h.set_ui([this] {
            origin = ui::cursor();
            if (const auto item = ui::rail("##steps", entries, ftk::theme::Hue::blue)) {
                clicked = static_cast<int>(*item);
            }
        });
        h.step(3);
    }

    // The middle of line `i`, a little in from the left of its text: steps
    // are 24 px tall, items 26.
    [[nodiscard]] glm::vec2 line(int i) const {
        float y = origin.y;
        for (int k = 0; k < i; ++k) y += entries[static_cast<std::size_t>(k)].item ? 26.0f : 24.0f;
        y += entries[static_cast<std::size_t>(i)].item ? 13.0f : 12.0f;
        return {origin.x + 40.0f, y};
    }
};

} // namespace

TEST_CASE("Clicking a rail item reports which item it is, wherever it shows", "[ui][kit]") {
    Fixture f;
    f.h.click_at(f.line(3));
    CHECK(f.clicked == 1);
    f.h.click_at(f.line(1));
    CHECK(f.clicked == 0);
    f.clicked = -1;
    f.h.click_at(f.line(4));
    CHECK(f.clicked == 0);
}

TEST_CASE("A rail's steps do nothing when clicked", "[ui][kit]") {
    Fixture f;
    f.h.click_at(f.line(0));
    f.h.click_at(f.line(2));
    CHECK(f.clicked == -1);
}
