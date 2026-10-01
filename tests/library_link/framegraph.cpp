// Links ftk-framegraph alone and whole (tests/CMakeLists.txt), so it builds
// only if everything in the library finds what it needs in the library and
// its own dependencies. Running it uses the part that needs no device: the
// graph's frame bookkeeping.

#include "ftk/base/log.hpp"
#include "ftk/framegraph/frame_graph.hpp"

int main() {
    ftk::log::init({.level = spdlog::level::warn});

    ftk::FrameGraph graph;
    graph.new_frame();

    ftk::log::shutdown();
    return 0;
}
