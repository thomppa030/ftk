// Links ftk-app-imgui alone and whole (tests/CMakeLists.txt), so it builds
// only if everything in the library finds what it needs in the library and
// its own dependencies. Running it opens a window on a GPU and takes a shot
// of the kit drawn with ImGui, which takes a GPU and a display: it is run by
// hand, not as a test, with the ImGui layer's files:
//
//     ftk-link-app-imgui <fonts directory> <imgui.frag.spv>

#include "ftk/app/imgui_app_window.hpp"
#include "ftk/base/log.hpp"
#include "ftk/ui/kit/button.hpp"

#include <cstdio>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <fonts directory> <imgui.frag.spv>\n", argv[0]);
        return 2;
    }
    ftk::log::init({.level = spdlog::level::warn});
    int drawn = 0;
    {
        auto made = ftk::ImGuiAppWindow::create({.title = "ftk-link-app-imgui",
                                                 .width = 320,
                                                 .height = 240,
                                                 .files = {.fonts = argv[1], .srgb_fragment = argv[2]}});
        if (!made) {
            std::fprintf(stderr, "%s\n", made.error().c_str());
            return 1;
        }
        auto shot = (*made)->shot(
            [&](float, float) {
                (void)ftk::ui::button("OK");
                ++drawn;
            },
            {160, 120});
        if (!shot) drawn = 0;
    }
    ftk::log::shutdown();
    return drawn > 0 ? 0 : 1;
}
