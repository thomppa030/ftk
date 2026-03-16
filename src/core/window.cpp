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
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

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

    glfwSetWindowUserPointer(window_, this);
    glfwSetFramebufferSizeCallback(window_, framebuffer_resize_callback);

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

void Window::set_title(std::string_view title) {
    glfwSetWindowTitle(window_, std::string{title}.c_str());
}

VkSurfaceKHR Window::create_surface(VkInstance instance) const {
    VkSurfaceKHR surface{};
    if (glfwCreateWindowSurface(instance, window_, nullptr, &surface) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create window surface");
    }
    return surface;
}

void Window::framebuffer_resize_callback(GLFWwindow* window, int width, int height) {
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (self) {
        self->framebuffer_resized_ = true;
        self->width_ = static_cast<uint32_t>(width);
        self->height_ = static_cast<uint32_t>(height);
    }
}

} // namespace fjell
