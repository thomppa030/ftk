#include "hub/hub_ui.hpp"
#include "core/log.hpp"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

// Minimal OpenGL loader — just the functions ImGui needs
// On Linux, GLFW exposes GL symbols via libGL.so
#include <GL/gl.h>

#include <cstdlib>
#include <string>
#include <unistd.h>

static constexpr int HUB_WIDTH = 480;
static constexpr int HUB_HEIGHT = 680;

static void launch_editor(const std::string& project_path) {
    pid_t pid = fork();
    if (pid == 0) {
        // Child process — exec the editor
        // Find fjell binary relative to fjell-hub
        char self[4096];
        auto len = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (len <= 0) _exit(1);
        self[len] = '\0';

        std::string hub_path(self);
        auto pos = hub_path.rfind('/');
        std::string editor_path = hub_path.substr(0, pos + 1) + "fjell";

        execl(editor_path.c_str(), "fjell", project_path.c_str(), nullptr);
        _exit(1); // exec failed
    }
    // Parent continues running the hub
}

int main() {
    fjell::log::init();

    if (!glfwInit()) {
        FJELL_CORE_CRITICAL("Failed to initialize GLFW");
        return EXIT_FAILURE;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

    auto* window = glfwCreateWindow(HUB_WIDTH, HUB_HEIGHT, "Fjell", nullptr, nullptr);
    if (!window) {
        FJELL_CORE_CRITICAL("Failed to create window");
        glfwTerminate();
        return EXIT_FAILURE;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // vsync

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // don't save imgui.ini

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    fjell::hub::HubUI hub_ui;

    float last_time = static_cast<float>(glfwGetTime());

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        float now = static_cast<float>(glfwGetTime());
        float dt = now - last_time;
        last_time = now;

        hub_ui.update_splash(dt);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        auto launch = hub_ui.draw(static_cast<float>(w), static_cast<float>(h));

        ImGui::Render();

        glViewport(0, 0, w, h);
        glClearColor(0.118f, 0.122f, 0.133f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);

        if (!launch.empty()) {
            launch_editor(launch);
            // Stay on hub — could also glfwSetWindowShouldClose(window, true)
        }
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    fjell::log::shutdown();
    return EXIT_SUCCESS;
}
