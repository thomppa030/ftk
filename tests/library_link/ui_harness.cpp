// Links ftk-ui-harness alone and whole (tests/CMakeLists.txt), so it builds
// only if everything in the library finds what it needs in the library and
// the UI core. Running it clicks a hit area and expects the click.

#include "ftk/test/ui_harness.hpp"

int main() {
    ftk::test::UiHarness h;
    bool clicked = false;
    h.set_ui([&] {
        auto& ui = h.ui();
        const ftk::ui::core::Rect at = ui.layout().next({100.0f, 30.0f});
        clicked = clicked || ui.interact(ui.id("button"), at).clicked;
        h.mark("button");
    });
    h.step();
    h.click("button");
    return clicked ? 0 : 1;
}
