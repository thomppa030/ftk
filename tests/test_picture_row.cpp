#include "kit_harness.hpp"

#include "ftk/ui/kit/picture_row.hpp"
#include "ftk/ui/kit/popup.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>

using ftk::test::KitHarness;
namespace ui = ftk::ui;

namespace {

// Two rows, the picked one tracked as a list's owner tracks it, with a menu
// on each row as a template of the user's has.
struct Rows {
    KitHarness h;
    int picked{0};
    int opened{-1};
    int double_clicked{-1};
    bool menu{false};

    Rows() {
        h.set_ui([this] {
            constexpr std::array<const char*, 2> NAMES{"Empty", "Harbour base"};
            for (int i = 0; i < 2; ++i) {
                const auto row = ui::picture_row({.id = NAMES[static_cast<std::size_t>(i)],
                                                  .icon = "?",
                                                  .name = NAMES[static_cast<std::size_t>(i)],
                                                  .detail = "A line about it"},
                                                 picked == i);
                h.mark(NAMES[static_cast<std::size_t>(i)]);
                if (row.clicked) picked = i;
                if (row.double_clicked) double_clicked = i;
                if (auto context = ui::ContextMenu("##row")) {
                    menu = true;
                    opened = i;
                }
            }
        });
        h.step(2);
    }
};

} // namespace

TEST_CASE("A picture row is picked by a click", "[ui][picture_row]") {
    Rows r;
    CHECK(r.picked == 0);
    r.h.click("Harbour base");
    CHECK(r.picked == 1);
    r.h.click("Empty");
    CHECK(r.picked == 0);
}

TEST_CASE("A picture row says when it is double-clicked", "[ui][picture_row]") {
    Rows r;
    r.h.double_click("Harbour base");
    CHECK(r.double_clicked == 1);
}

TEST_CASE("A picture row is the item its owner's menu opens on", "[ui][picture_row]") {
    Rows r;
    r.h.click("Harbour base", ui::MouseButton::Right);
    r.h.step();
    CHECK(r.menu);
    CHECK(r.opened == 1);
}

TEST_CASE("Picture rows stack, each as wide as the space", "[ui][picture_row]") {
    Rows r;
    const auto first = r.h.rect("Empty");
    const auto second = r.h.rect("Harbour base");
    CHECK(second.min.y >= first.max.y);
    CHECK(first.width() == second.width());
    CHECK(first.height() > 36.0f);
}
