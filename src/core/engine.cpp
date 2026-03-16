#include "core/engine.hpp"
#include "core/log.hpp"
#include "renderer/material.hpp"
#include "renderer/model.hpp"
#include "renderer/texture.hpp"

#include <GLFW/glfw3.h>

#include <filesystem>

namespace fjell {

Engine::Engine(std::string_view title, uint32_t width, uint32_t height)
    : window_{title, width, height}
    , camera_{{0.0f, 5.0f, 30.0f}}
    , vulkan_context_{window_} {
    window_.set_input(&input_);
    input_.attach(window_.handle());
    input_.set_cursor_captured(window_.handle(), true);

    resource_manager_ = std::make_unique<ResourceManager>(
        vulkan_context_.device(), vulkan_context_.physical_device(),
        vulkan_context_.command_pool(), vulkan_context_.graphics_queue(),
        vulkan_context_.material_pool(), vulkan_context_.material_layout());

    setup_scene();
    FJELL_CORE_INFO("Engine initialized");
}

Engine::~Engine() {
    vulkan_context_.wait_idle();
    scene_ = {};
    resource_manager_.reset();
    FJELL_CORE_INFO("Engine shut down");
}

void Engine::setup_scene() {
    // Load a test texture for materials
    auto* tex = resource_manager_->load_texture("assets/textures/test.png");
    auto* default_mat = resource_manager_->create_material("default", tex);

    // Try loading glTF models
    const char* model_paths[] = {
        "assets/models/scene.gltf", "assets/models/scene.glb",
        "assets/models/cube.glb"
    };

    for (auto* path : model_paths) {
        if (std::filesystem::exists(path)) {
            auto* model = resource_manager_->load_model(path);

            // If model has its own textures, create a material from the first one
            Material* mat = default_mat;
            if (!model->textures().empty()) {
                mat = resource_manager_->create_material(
                    std::string(path) + "_mat",
                    const_cast<Texture*>(model->textures()[0].get()));
            }

            scene_.add(model, mat);
            FJELL_CORE_INFO("Added model to scene: {}", path);
            break;
        }
    }
}

void Engine::run() {
    FJELL_CORE_INFO("Entering main loop");
    while (!window_.should_close()) {
        input_.update();
        window_.poll_events();
        clock_.tick();

        if (input_.is_key_pressed(GLFW_KEY_ESCAPE)) {
            input_.set_cursor_captured(window_.handle(), !input_.is_cursor_captured());
        }

        camera_.update(input_, clock_.delta_time());

        float aspect = static_cast<float>(window_.width()) /
                       static_cast<float>(std::max(window_.height(), 1u));
        auto view = camera_.view_matrix();
        auto projection = camera_.projection_matrix(aspect);

        vulkan_context_.draw_frame(view, projection, &scene_);
    }
    FJELL_CORE_INFO("Exiting main loop");
}

} // namespace fjell
