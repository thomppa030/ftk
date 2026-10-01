// Links ftk-imgui-harness alone and whole (tests/CMakeLists.txt), so it builds
// only if everything in the library finds what it needs in the library and
// its own dependencies. Running it steps a frame of a button and clicks it.

#include "ftk/test/imgui_harness.hpp"
#include "ftk/ui/kit/button.hpp"

int main() {
    ftk::test::ImGuiHarness h;
    int clicks = 0;
    h.set_ui([&] {
        if (ftk::ui::button("OK")) ++clicks;
        h.mark("ok");
    });
    h.step();
    h.click("ok");
    return clicks == 1 ? 0 : 1;
}
