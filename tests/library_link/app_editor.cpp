// Links ftk-app-editor alone and whole (tests/CMakeLists.txt), so it builds
// only if everything in the library finds what it needs in the library and
// its own dependencies. Running it opens a standalone window on a GPU and
// draws the kit in it for a few frames, which takes a GPU and a display: it
// is run by hand, not as a test, with the kit's fonts:
//
//     ftk-link-app-editor <fonts directory>

#include "ftk/app/standalone_window.hpp"
#include "ftk/base/log.hpp"
#include "ftk/gpu/command_list.hpp"
#include "ftk/gpu/device.hpp"
#include "ftk/gpu/frame.hpp"
#include "ftk/gpu/window.hpp"
#include "ftk/ui/core_kit.hpp"
#include "ftk/ui/kit/button.hpp"

#include <cstdio>
#include <filesystem>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <fonts directory>\n", argv[0]);
        return 2;
    }
    ftk::log::init({.level = spdlog::level::warn});
    int drawn = 0;
    {
        // The device is made for a window of its own, as the editor's is.
        ftk::Window main_window("ftk-link-app-editor", 320, 240);
        auto made = ftk::gpu::Device::create(main_window);
        if (!made) {
            std::fprintf(stderr, "%s\n", made.error().c_str());
            return 1;
        }
        ftk::gpu::Device& device = **made;
        const std::filesystem::path fonts = argv[1];
        ftk::StandaloneWindow window(device, "ftk-link-app-editor", 480, 320,
                                     [&](ftk::ui::core::Context& ui) { (void)ftk::ui::use_kit_fonts(ui, fonts); });
        // Laid out between frames and drawn into the program's frame, as an
        // editor draws its windows of their own.
        for (int frame = 0; frame < 10; ++frame) {
            window.frame([&](float, float) {
                (void)ftk::ui::button("OK");
                ++drawn;
            });
            ftk::gpu::Frame& gpu_frame = device.begin_frame();
            ftk::gpu::CommandList& cmd = gpu_frame.commands(ftk::gpu::Queue::graphics);
            window.record(gpu_frame, cmd);
            gpu_frame.submit(cmd);
            (void)device.end_frame(gpu_frame);
        }
        device.wait_idle();
    }
    ftk::log::shutdown();
    return drawn > 0 ? 0 : 1;
}
