#include "kit_harness.hpp"

#include "ftk/ui/kit/drop_cue.hpp"
#include "ftk/ui/theme.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include <vector>

using ftk::test::KitHarness;
namespace ui = ftk::ui;

namespace {

// The middle of each shape the last frame drew in `colour`.
std::vector<glm::vec2> drawn_in(const KitHarness& h, ftk::ui::Color colour) {
    const uint32_t packed = ftk::ui::core::pack_colour(colour);
    std::vector<glm::vec2> centres;
    for (const auto& root : h.output().roots) {
        for (const auto& p : root.draw->primitives) {
            if (p.colour == packed) centres.push_back((p.min + p.max) * 0.5f);
        }
    }
    return centres;
}

} // namespace

TEST_CASE("The drop rim outlines its area in dashes of the accent, on all four sides and inside it", "[ui][kit]") {
    KitHarness h;
    const glm::vec2 min{40.0f, 60.0f};
    const glm::vec2 max{340.0f, 260.0f};
    h.set_ui([&] { ui::drop_rim(min, max); });
    h.step(2);
    const std::vector<glm::vec2> dashes = drawn_in(h, ftk::theme::accent());
    // Dashes, not one line a side.
    REQUIRE(dashes.size() > 8);
    bool top = false, bottom = false, left = false, right = false;
    for (const glm::vec2 at : dashes) {
        CHECK((at.x >= min.x && at.x <= max.x && at.y >= min.y && at.y <= max.y));
        top = top || at.y < min.y + 2.0f;
        bottom = bottom || at.y > max.y - 2.0f;
        left = left || at.x < min.x + 2.0f;
        right = right || at.x > max.x - 2.0f;
    }
    CHECK((top && bottom && left && right));
}

TEST_CASE("The drop plate sits in the middle of its bar, its words on the accent", "[ui][kit]") {
    KitHarness h;
    const glm::vec2 min{20.0f, 10.0f};
    const glm::vec2 max{620.0f, 38.0f};
    h.set_ui([&] { ui::drop_plate(min, max, "assets/props/barrels"); });
    h.step(2);
    const std::vector<glm::vec2> plate = drawn_in(h, ftk::theme::accent());
    REQUIRE(plate.size() == 1);
    CHECK(std::abs(plate.front().x - (min.x + max.x) * 0.5f) <= 1.0f);
    CHECK(std::abs(plate.front().y - (min.y + max.y) * 0.5f) <= 1.0f);
    const std::vector<glm::vec2> words = drawn_in(h, ftk::theme::text_on_accent());
    REQUIRE_FALSE(words.empty());
    for (const glm::vec2 at : words) CHECK((at.y > min.y && at.y < max.y));
}
