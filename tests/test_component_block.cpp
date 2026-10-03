#include "kit_harness.hpp"

#include "ftk/ui/kit/component_block.hpp"
#include "ftk/ui/kit/item.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/theme.hpp"

#include <catch2/catch_test_macros.hpp>

using ftk::test::KitHarness;
namespace ui = ftk::ui;

TEST_CASE("A component block folds from its header and asks to be removed from its trash icon", "[ui][kit]") {
    KitHarness h;
    bool open = false;
    bool remove = false;
    int removals = 0;
    h.set_ui([&] {
        remove = false;
        open = ui::component_block("Collider", ftk::theme::Hue::coral, remove);
        h.mark("trash");  // the last item is the trash icon
        if (remove) ++removals;
    });
    h.step(2);
    CHECK(open);
    const glm::vec2 trash_min = h.rect("trash").min;
    const float y = h.rect("trash").centre().y;

    // The header left of the trash icon folds the block.
    h.click_at({trash_min.x - 40.0f, y});
    CHECK_FALSE(open);
    h.click_at({trash_min.x - 40.0f, y});
    CHECK(open);

    // The trash icon asks for removal and leaves the fold alone.
    h.click("trash");
    CHECK(removals == 1);
    CHECK(open);
}

TEST_CASE("A block with a switch turns off without folding or removing", "[ui][kit]") {
    KitHarness h;
    bool enabled = true;
    bool removed = false;
    bool open = false;
    glm::vec2 header_max{};
    float frame = 0.0f;
    h.set_ui([&] {
        frame = ui::frame_height();
        open = ui::component_block("Gravity", ftk::theme::Hue::periwinkle, removed, enabled);
        header_max = ui::item_max();  // the trash icon, last in the header
    });
    h.step(2);
    // The switch sits just before the trash icon.
    h.click_at({header_max.x - frame - ftk::theme::GAP_S - 14.0f, header_max.y - frame * 0.5f});
    h.step();
    CHECK_FALSE(enabled);
    CHECK(open);
    CHECK_FALSE(removed);
}
