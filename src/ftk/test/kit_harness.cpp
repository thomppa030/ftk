#include "ftk/test/kit_harness.hpp"

#include "ftk/ui/core_kit.hpp"
#include "ftk/ui/kit/item.hpp"

#include <utility>

namespace ftk::test {

KitHarness::KitHarness(glm::vec2 size) : UiHarness(size, 1.0f, false) {
    ui::use_kit_fonts(ui(), FTK_KIT_HARNESS_FONTS);
}

void KitHarness::set_ui(std::function<void()> draw) {
    UiHarness::set_ui([this, draw = std::move(draw)] {
        if (ui::begin_plain_window("##harness", {0.0f, 0.0f}, size()) && draw) draw();
        ui::end_plain_window();
    });
}

void KitHarness::mark(const std::string& name) {
    UiHarness::mark(name, {.min = ui::item_min(), .max = ui::item_max()});
}

} // namespace ftk::test
