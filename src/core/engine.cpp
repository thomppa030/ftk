#include "core/engine.hpp"
#include "core/log.hpp"

#include <GLFW/glfw3.h>

namespace fjell {

Engine::Engine(std::string_view title, uint32_t width, uint32_t height)
    : window_{title, width, height}
    , camera_{{0.0f, 5.0f, 30.0f}}
    , vulkan_context_{window_} {
    window_.set_input(&input_);
    input_.attach(window_.handle());
    input_.set_cursor_captured(window_.handle(), true);
    FJELL_CORE_INFO("Engine initialized");
}

Engine::~Engine() {
    vulkan_context_.wait_idle();
    FJELL_CORE_INFO("Engine shut down");
}

void Engine::run() {
    FJELL_CORE_INFO("Entering main loop");
    while (!window_.should_close()) {
        input_.update();
        window_.poll_events();
        clock_.tick();

        // Toggle cursor capture with Escape
        if (input_.is_key_pressed(GLFW_KEY_ESCAPE)) {
            input_.set_cursor_captured(window_.handle(), !input_.is_cursor_captured());
        }

        camera_.update(input_, clock_.delta_time());

        float aspect = static_cast<float>(window_.width()) /
                       static_cast<float>(std::max(window_.height(), 1u));
        auto view = camera_.view_matrix();
        auto projection = camera_.projection_matrix(aspect);

        vulkan_context_.draw_frame(view, projection);
    }
    FJELL_CORE_INFO("Exiting main loop");
}

} // namespace fjell
