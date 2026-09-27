// Links fjell-gpu alone and whole (tests/CMakeLists.txt), so it builds only
// if everything in the library finds what it needs in the library and its own
// dependencies. Running it opens a window, brings a device up and allocates a
// buffer, which takes a GPU and a display: it is run by hand, not as a test.

#include "core/log.hpp"
#include "renderer/gpu/buffer.hpp"
#include "renderer/gpu/gpu_core.hpp"
#include "renderer/gpu/window.hpp"

int main() {
    fjell::log::init({.level = spdlog::level::warn});
    bool ran = false;
    {
        fjell::Window window("fjell-link-gpu", 320, 240);
        fjell::GpuCore gpu(window);
        const auto buffer = fjell::Buffer::create(
            gpu.allocator(), 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO);
        gpu.wait_idle();
        ran = buffer.has_value();
    }
    fjell::log::shutdown();
    return ran ? 0 : 1;
}
