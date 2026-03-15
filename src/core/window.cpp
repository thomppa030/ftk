#include "core/window.hpp"
#include "core/log.hpp"

#include <stdexcept>
#include <string>

namespace fjell {

Window::Window(std::string_view title, uint32_t width, uint32_t height)
    : width_{width}, height_{height} {
    if (!glfwInit()) {
        throw std::runtime_error("Failed to initialize GLFW");
    }
    FJELL_CORE_DEBUG("GLFW initialized");

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

    window_ = glfwCreateWindow(
        static_cast<int>(width_),
        static_cast<int>(height_),
        std::string{title}.c_str(),
        nullptr, nullptr
    );

    if (!window_) {
        glfwTerminate();
        throw std::runtime_error("Failed to create GLFW window");
    }
    FJELL_CORE_INFO("Window created: {}x{}", width_, height_);
}

Window::~Window() {
    if (window_) {
        glfwDestroyWindow(window_);
    }
    glfwTerminate();
    FJELL_CORE_DEBUG("Window destroyed");
}

bool Window::should_close() const {
    return glfwWindowShouldClose(window_);
}

void Window::poll_events() {
    glfwPollEvents();
}

VkSurfaceKHR Window::create_surface(VkInstance instance) const {
    VkSurfaceKHR surface{};
    if (glfwCreateWindowSurface(instance, window_, nullptr, &surface) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create window surface");
    }
    return surface;
}

} // namespace fjell
