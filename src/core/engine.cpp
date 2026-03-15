#include "core/engine.hpp"

namespace fjell {

Engine::Engine(std::string_view title, uint32_t width, uint32_t height)
    : window_{title, width, height}
    , vulkan_context_{window_} {
}

Engine::~Engine() {
    vulkan_context_.wait_idle();
}

void Engine::run() {
    while (!window_.should_close()) {
        window_.poll_events();
        vulkan_context_.draw_frame();
    }
}

} // namespace fjell
