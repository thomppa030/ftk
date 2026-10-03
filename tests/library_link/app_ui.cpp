// Links ftk-app-ui alone and whole (tests/CMakeLists.txt), so it builds only
// if everything in the library finds what it needs in the library and its
// own dependencies: the kit on the UI core, with no ImGui. Running it draws
// a frame of a button in a plain window, headless, and expects shapes.

#include "ftk/ui/core/context.hpp"
#include "ftk/ui/core_kit.hpp"
#include "ftk/ui/kit/button.hpp"

int main() {
    ftk::ui::core::Context ui;
    ftk::ui::core::set_current(&ui);
    ui.begin_frame(0.0);
    ui.begin_root(1, {320.0f, 240.0f}, 1.0f);
    if (ftk::ui::begin_plain_window("##link", {0.0f, 0.0f}, {320.0f, 240.0f})) (void)ftk::ui::button("OK");
    ftk::ui::end_plain_window();
    ui.end_root();
    const ftk::ui::core::FrameOutput& out = ui.end_frame();
    return !out.roots.empty() && !out.roots[0].draw->primitives.empty() ? 0 : 1;
}
