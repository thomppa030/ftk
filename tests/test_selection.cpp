#include "ftk/ui/kit/selection.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <vector>

using ftk::ui::Modifiers;
using ftk::ui::Selection;

namespace {

constexpr std::array<uint32_t, 6> SHOWN{10, 20, 30, 40, 50, 60};
constexpr Modifiers PLAIN{};
constexpr Modifiers CTRL{.ctrl = true};
constexpr Modifiers SHIFT{.shift = true};

std::vector<uint32_t> picked(const Selection<uint32_t>& selection) {
    return {selection.keys().begin(), selection.keys().end()};
}

} // namespace

TEST_CASE("A click picks one alone, and Ctrl-click adds and takes away", "[ui][selection]") {
    Selection<uint32_t> selection;
    selection.click(30, PLAIN, SHOWN);
    selection.click(10, PLAIN, SHOWN);
    CHECK(picked(selection) == std::vector<uint32_t>{10});

    selection.click(40, CTRL, SHOWN);
    selection.click(60, CTRL, SHOWN);
    CHECK(picked(selection) == std::vector<uint32_t>{10, 40, 60});
    CHECK(selection.primary() == 60U);

    // Taking away the primary hands it to the one picked last of those left.
    selection.click(60, CTRL, SHOWN);
    CHECK(picked(selection) == std::vector<uint32_t>{10, 40});
    CHECK(selection.primary() == 40U);
    CHECK_FALSE(selection.contains(60));
}

TEST_CASE("Shift-click picks the run from the last one clicked, in the order shown", "[ui][selection]") {
    Selection<uint32_t> selection;
    selection.click(20, PLAIN, SHOWN);
    selection.click(50, SHIFT, SHOWN);
    CHECK(picked(selection) == std::vector<uint32_t>{20, 30, 40, 50});
    CHECK(selection.primary() == 50U);

    // The anchor stays where it was clicked, so the run can run back past it.
    selection.click(10, SHIFT, SHOWN);
    CHECK(picked(selection) == std::vector<uint32_t>{10, 20});

    // A Ctrl-click moves the anchor.
    selection.click(40, CTRL, SHOWN);
    selection.click(60, SHIFT, SHOWN);
    CHECK(picked(selection) == std::vector<uint32_t>{40, 50, 60});

    // With nothing clicked before, or an anchor no longer shown, it picks one.
    Selection<uint32_t> fresh;
    fresh.click(30, SHIFT, SHOWN);
    CHECK(picked(fresh) == std::vector<uint32_t>{30});
    const std::array<uint32_t, 2> fewer{50, 60};
    fresh.click(60, SHIFT, fewer);
    CHECK(picked(fresh) == std::vector<uint32_t>{60});
}

TEST_CASE("A selection keeps only what is still there", "[ui][selection]") {
    Selection<uint32_t> selection;
    selection.click(10, PLAIN, SHOWN);
    selection.click(40, CTRL, SHOWN);
    selection.click(50, CTRL, SHOWN);
    selection.keep_if([](uint32_t key) { return key != 50; });
    CHECK(picked(selection) == std::vector<uint32_t>{10, 40});
    CHECK(selection.primary() == 40U);

    selection.clear();
    CHECK(selection.empty());
    CHECK_FALSE(selection.primary().has_value());
}
