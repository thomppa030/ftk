// Links fjell-framegraph alone and whole (tests/CMakeLists.txt), so it builds
// only if everything in the library finds what it needs in the library and
// its own dependencies. Running it uses the part that needs no device: the
// graph's frame bookkeeping.

#include "core/log.hpp"
#include "renderer/frame_graph.hpp"

int main() {
    fjell::log::init({.level = spdlog::level::warn});

    fjell::FrameGraph graph;
    graph.new_frame();

    fjell::log::shutdown();
    return 0;
}
