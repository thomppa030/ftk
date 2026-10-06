#include "kit_harness.hpp"

#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/picture_strip.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>

using ftk::test::KitHarness;
namespace ui = ftk::ui;

TEST_CASE("A picture strip outlines the one shown and says which is clicked", "[ui][kit]") {
    KitHarness h;
    const ui::StripPicture pictures[] = {{.name = "moss"}, {.name = "bark"}, {.name = "slate"}};
    std::size_t shown = 0;
    h.set_ui([&] {
        if (const auto picked = ui::picture_strip("##textures", pictures, shown)) shown = *picked;
    });
    h.step(2);
    REQUIRE(h.find("moss", ui::core::Role::Button) != nullptr);
    CHECK(h.find("moss", ui::core::Role::Button)->state.selected);
    CHECK_FALSE(h.find("slate", ui::core::Role::Button)->state.selected);

    h.click_at(h.find("slate", ui::core::Role::Button)->rect.centre());
    CHECK(shown == 2);
    h.step();
    CHECK(h.find("slate", ui::core::Role::Button)->state.selected);
    CHECK_FALSE(h.find("moss", ui::core::Role::Button)->state.selected);
}
