#include "core/engine.hpp"
#include "core/log.hpp"

namespace fjell {

Engine::Engine(std::string_view title, uint32_t width, uint32_t height)
    : window_{title, width, height}
    , vulkan_context_{window_} {
    FJELL_CORE_INFO("Engine initialized");
}

Engine::~Engine() {
    vulkan_context_.wait_idle();
    FJELL_CORE_INFO("Engine shut down");
}

void Engine::run() {
    FJELL_CORE_INFO("Entering main loop");
    while (!window_.should_close()) {
        window_.poll_events();
        vulkan_context_.draw_frame();
    }
    FJELL_CORE_INFO("Exiting main loop");
}

} // namespace fjell
