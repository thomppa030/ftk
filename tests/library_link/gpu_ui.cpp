// Links ftk-gpu-ui alone and whole (tests/CMakeLists.txt), so it builds only
// if everything in the library finds what it needs in the library and its own
// dependencies. Running it opens a window, brings a device up and makes the
// UI renderer, its pipelines and shaders, which takes a GPU and a display: it
// is run by hand, not as a test.

#include "ftk/base/log.hpp"
#include "ftk/gpu/device.hpp"
#include "ftk/gpu/ui_renderer.hpp"
#include "ftk/gpu/window.hpp"

#include <cstdio>

int main() {
    ftk::log::init({.level = spdlog::level::warn});
    ftk::Window window("ftk-link-gpu-ui", 320, 240);
    auto device = ftk::gpu::Device::create(window);
    if (!device) {
        std::fprintf(stderr, "%s\n", device.error().c_str());
        return 1;
    }
    auto renderer = ftk::gpu::UiRenderer::create(**device, ftk::gpu::Format::bgra8_srgb);
    if (!renderer) {
        std::fprintf(stderr, "%s\n", renderer.error().c_str());
        return 1;
    }
    (*device)->wait_idle();
    return 0;
}
