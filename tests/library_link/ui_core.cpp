// Links ftk-ui-core alone and whole (tests/CMakeLists.txt), so it builds only
// if everything in the library finds what it needs in the library and its
// own dependencies: no ImGui, no GPU. Running it draws one empty frame and
// expects it to wait for input.

#include "ftk/ui/core/context.hpp"

int main() {
    ftk::ui::core::Context ui;
    ui.begin_frame(0.0);
    ui.begin_root(1, {640.0f, 480.0f}, 1.0f);
    ui.end_root();
    return ui.end_frame().next.waits_for_input() ? 0 : 1;
}
