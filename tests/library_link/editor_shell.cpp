// Links fjell-editor-shell alone and whole (tests/CMakeLists.txt), so it
// builds only if everything in the library finds what it needs in the library
// and its own dependencies. Running it opens a standalone window on a GPU and
// draws the kit in it for a few frames, which takes a GPU and a display: it is
// run by hand, not as a test, with the ImGui layer's files:
//
//     fjell-link-editor-shell <fonts directory> <imgui.frag.spv>

#include "core/log.hpp"
#include "renderer/gpu/gpu_core.hpp"
#include "renderer/gpu/window.hpp"
#include "ui/imgui_layer.hpp"
#include "ui/kit/button.hpp"
#include "ui/standalone_window.hpp"

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
        fjell::Window main_window("fjell-link-editor-shell", 320, 240);
        fjell::GpuCore gpu(main_window);
        fjell::StandaloneWindow window(gpu, "fjell-link-editor-shell", 480, 320,
                                       {.fonts = argv[1], .srgb_fragment = argv[2]});
        for (int frame = 0; frame < 10; ++frame) {
            window.frame([&](float, float) {
                (void)fjell::ui::button("OK");
                ++drawn;
            });
        }
        gpu.wait_idle();
    }
    fjell::log::shutdown();
    return drawn > 0 ? 0 : 1;
}
