// Links ftk-app alone and whole (tests/CMakeLists.txt), so it builds only if
// everything in the library finds what it needs in the library and its own
// dependencies: no ImGui. Running it opens a window on a GPU and takes a shot
// of a frame of the UI core, which takes a GPU and a display: it is run by
// hand, not as a test.

#include "ftk/app/app_window.hpp"
#include "ftk/base/log.hpp"
#include "ftk/ui/core/context.hpp"

#include <cstdio>

int main() {
    ftk::log::init({.level = spdlog::level::warn});
    int code = 1;
    {
        auto made = ftk::AppWindow::create({.title = "ftk-link-app", .width = 320, .height = 240});
        if (!made) {
            std::fprintf(stderr, "%s\n", made.error().c_str());
            return 1;
        }
        auto shot = (*made)->shot(
            [](ftk::ui::core::Context& ui, glm::vec2 size) {
                ui.draw_list().rect_filled({8.0f, 8.0f}, size - 8.0f, {1.0f, 1.0f, 1.0f, 1.0f}, 4.0f);
            },
            {64, 48});
        // The middle of the frame is the white rectangle.
        if (shot && shot->pixels[(24 * 64 + 32) * 4] == 255) code = 0;
    }
    ftk::log::shutdown();
    return code;
}
