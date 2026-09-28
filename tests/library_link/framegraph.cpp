// Links fjell-framegraph alone and whole (tests/CMakeLists.txt), so it builds
// only if everything in the library finds what it needs in the library and
// its own dependencies. Running it uses the parts that need no device: the
// graph's frame bookkeeping and the descriptor cache's key.

#include "core/log.hpp"
#include "gpu/vulkan/frame_descriptor_cache.hpp"
#include "renderer/frame_graph.hpp"

int main() {
    fjell::log::init({.level = spdlog::level::warn});

    fjell::FrameGraph graph;
    graph.new_frame();

    const fjell::FrameCacheKey first;
    const fjell::FrameCacheKey second;
    const fjell::FrameCacheKeyHash hash;
    const bool ran = first == second && hash(first) == hash(second);

    fjell::log::shutdown();
    return ran ? 0 : 1;
}
