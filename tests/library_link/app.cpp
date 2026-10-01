// Links ftk-app alone and whole (tests/CMakeLists.txt), so it builds only if
// everything in the library finds what it needs in the library and its own
// dependencies. Running it opens a standalone window on a GPU and draws the
// kit in it for a few frames, which takes a GPU and a display: it is run by
// hand, not as a test, with the ImGui layer's files:
//
//     ftk-link-app <fonts directory> <imgui.frag.spv>

#include "ftk/app/imgui_layer.hpp"
#include "ftk/app/standalone_window.hpp"
#include "ftk/base/log.hpp"
#include "ftk/gpu/device.hpp"
#include "ftk/gpu/window.hpp"
#include "ftk/ui/kit/button.hpp"

#include <cstdio>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <fonts directory> <imgui.frag.spv>\n", argv[0]);
        return 2;
    }
    fjell::log::init({.level = spdlog::level::warn});
    int drawn = 0;
    {
        // The device is made for a window of its own, as the editor's is.
        fjell::Window main_window("ftk-link-app", 320, 240);
        auto made = fjell::gpu::Device::create(main_window);
        if (!made) {
            std::fprintf(stderr, "%s\n", made.error().c_str());
            return 1;
        }
        fjell::gpu::Device& device = **made;
        fjell::StandaloneWindow window(device, "ftk-link-app", 480, 320,
                                       {.fonts = argv[1], .srgb_fragment = argv[2]});
        for (int frame = 0; frame < 10; ++frame) {
            window.frame([&](float, float) {
                (void)fjell::ui::button("OK");
                ++drawn;
            });
        }
        device.wait_idle();
    }
    fjell::log::shutdown();
    return drawn > 0 ? 0 : 1;
}
