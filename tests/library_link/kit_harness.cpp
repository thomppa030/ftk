// Links ftk-kit-harness alone and whole (tests/CMakeLists.txt), so it builds
// only if everything in the library finds what it needs in the library, the
// UI core's harness and the kit. Running it clicks the kit's button and
// expects the click.

#include "ftk/test/kit_harness.hpp"
#include "ftk/ui/kit/button.hpp"

int main() {
    ftk::test::KitHarness h;
    bool clicked = false;
    h.set_ui([&] {
        clicked = clicked || ftk::ui::button("Bake");
        h.mark("bake");
    });
    h.step(2);
    h.click("bake");
    return clicked ? 0 : 1;
}
