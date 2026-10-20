#include "kit_harness.hpp"

#include "ftk/ui/kit/selection_box.hpp"
#include "ftk/ui/theme.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using ftk::test::KitHarness;
namespace ui = ftk::ui;

namespace {

struct Drawn {
    glm::vec2 min;
    glm::vec2 max;
};

// What the last frame drew in `colour`.
std::vector<Drawn> drawn_in(const KitHarness& h, ftk::ui::Color colour) {
    const uint32_t packed = ftk::ui::core::pack_colour(colour);
    std::vector<Drawn> shapes;
    for (const auto& root : h.output().roots) {
        for (const auto& p : root.draw->primitives) {
            if (p.colour == packed) shapes.push_back({p.min, p.max});
        }
    }
    return shapes;
}

} // namespace

TEST_CASE("A selection box washes its area in the accent and outlines it, whichever way it was dragged",
          "[ui][kit]") {
    // Dragged up and to the left: the corners come in the other order.
    const glm::vec2 pressed{340.0f, 260.0f};
    const glm::vec2 now{40.0f, 60.0f};
    KitHarness h;
    h.set_ui([&] { ui::selection_box(pressed, now); });
    h.step(2);

    ftk::ui::Color wash = ftk::theme::accent();
    wash.a *= 0.08f;
    const std::vector<Drawn> washed = drawn_in(h, wash);
    REQUIRE(washed.size() == 1);
    CHECK(washed.front().min == now);
    CHECK(washed.front().max == pressed);

    const std::vector<Drawn> outlined = drawn_in(h, ftk::theme::accent());
    REQUIRE_FALSE(outlined.empty());
    for (const Drawn& edge : outlined) {
        CHECK((edge.min.x >= now.x - 1.0f && edge.max.x <= pressed.x + 1.0f));
        CHECK((edge.min.y >= now.y - 1.0f && edge.max.y <= pressed.y + 1.0f));
    }
}
