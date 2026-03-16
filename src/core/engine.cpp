#include "core/engine.hpp"
#include "core/log.hpp"
#include "renderer/material.hpp"
#include "renderer/model.hpp"
#include "renderer/texture.hpp"

#include <imgui.h>
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

    imgui_layer_ = std::make_unique<ImGuiLayer>(
        window_.handle(), vulkan_context_.instance(),
        vulkan_context_.physical_device(), vulkan_context_.device(),
        vulkan_context_.graphics_family(), vulkan_context_.graphics_queue(),
        vulkan_context_.render_pass(), vulkan_context_.image_count());

    vulkan_context_.set_overlay_callback([this](VkCommandBuffer cmd) {
        imgui_layer_->render(cmd);
    });

    resource_manager_ = std::make_unique<ResourceManager>(
        vulkan_context_.device(), vulkan_context_.physical_device(),
        vulkan_context_.command_pool(), vulkan_context_.graphics_queue(),
        vulkan_context_.material_pool(), vulkan_context_.material_layout());

    // Load default texture
    resource_manager_->load_texture("assets/textures/test.png");

    scan_models();
    FJELL_CORE_INFO("Engine initialized");
}

Engine::~Engine() {
    vulkan_context_.wait_idle();
    vulkan_context_.set_overlay_callback(nullptr);
    scene_ = {};
    resource_manager_.reset();
    imgui_layer_.reset();
    FJELL_CORE_INFO("Engine shut down");
}

void Engine::scan_models() {
    available_models_.clear();
    const auto models_dir = std::filesystem::path("assets/models");
    if (!std::filesystem::exists(models_dir)) return;

    for (const auto& entry : std::filesystem::directory_iterator(models_dir)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        if (ext == ".gltf" || ext == ".glb") {
            available_models_.push_back(entry.path().string());
        }
    }
    std::sort(available_models_.begin(), available_models_.end());
    FJELL_CORE_INFO("Found {} model(s) in assets/models/", available_models_.size());
}

void Engine::add_model_to_scene(const std::string& path) {
    vulkan_context_.wait_idle();

    auto* model = resource_manager_->load_model(path);
    auto* tex = resource_manager_->load_texture("assets/textures/test.png");
    auto* mat = resource_manager_->create_material("default", tex);

    if (!model->textures().empty()) {
        mat = resource_manager_->create_material(
            path + "_mat",
            const_cast<Texture*>(model->textures()[0].get()));
    }

    auto filename = std::filesystem::path(path).stem().string();
    scene_.add(model, mat, {}, filename);
    FJELL_CORE_INFO("Added model to scene: {}", path);
}

void Engine::draw_ui() {
    // FPS calculation
    frame_count_++;
    fps_timer_ += clock_.delta_time();
    if (fps_timer_ >= 0.5f) {
        fps_ = static_cast<float>(frame_count_) / fps_timer_;
        frame_time_ = fps_timer_ / static_cast<float>(frame_count_) * 1000.0f;
        frame_count_ = 0;
        fps_timer_ = 0.0f;
    }

    ImGuiWindowFlags overlay_flags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoMove;

    // Performance overlay — top right
    const float pad = 12.0f;
    ImVec2 perf_pos = {ImGui::GetMainViewport()->WorkPos.x +
                       ImGui::GetMainViewport()->WorkSize.x - pad,
                       ImGui::GetMainViewport()->WorkPos.y + pad};
    ImGui::SetNextWindowPos(perf_pos, ImGuiCond_Always, {1.0f, 0.0f});
    ImGui::SetNextWindowBgAlpha(0.75f);

    if (ImGui::Begin("##Performance", nullptr, overlay_flags)) {
        ImGui::TextColored({0.50f, 0.60f, 0.82f, 1.0f}, "Fjell Engine");
        ImGui::Separator();
        ImGui::Text("FPS: %.0f", fps_);
        ImGui::Text("Frame: %.2f ms", frame_time_);
        ImGui::Spacing();
        auto pos = camera_.position();
        ImGui::Text("Camera: %.1f, %.1f, %.1f", pos.x, pos.y, pos.z);
    }
    ImGui::End();

    // Model browser — left side
    ImGui::SetNextWindowPos(
        {ImGui::GetMainViewport()->WorkPos.x + pad,
         ImGui::GetMainViewport()->WorkPos.y + pad},
        ImGuiCond_Always, {0.0f, 0.0f});
    ImGui::SetNextWindowSizeConstraints({220, 100}, {320, 600});
    ImGui::SetNextWindowBgAlpha(0.85f);

    ImGuiWindowFlags panel_flags =
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing;

    if (ImGui::Begin("Scene", nullptr, panel_flags)) {
        // Available models
        ImGui::TextColored({0.50f, 0.60f, 0.82f, 1.0f}, "Models");
        ImGui::Separator();

        if (available_models_.empty()) {
            ImGui::TextDisabled("No models found");
        } else {
            for (const auto& model_path : available_models_) {
                auto name = std::filesystem::path(model_path).filename().string();
                if (ImGui::Button(name.c_str(), {-1, 0})) {
                    add_model_to_scene(model_path);
                }
            }
        }

        if (ImGui::Button("Refresh", {-1, 0})) {
            scan_models();
        }

        ImGui::Spacing();
        ImGui::Spacing();

        // Scene objects
        ImGui::TextColored({0.50f, 0.60f, 0.82f, 1.0f}, "Objects (%d)", scene_.object_count());
        ImGui::Separator();

        remove_pending_ = -1;
        auto& objects = scene_.objects();
        for (int i = 0; i < static_cast<int>(objects.size()); ++i) {
            auto& obj = objects[i];
            ImGui::PushID(i);

            bool header_open = ImGui::CollapsingHeader(obj.name.c_str());

            // Remove button on same line as header
            ImGui::SameLine(ImGui::GetWindowWidth() - 30);
            if (ImGui::SmallButton("X")) {
                remove_pending_ = i;
            }

            if (header_open) {
                ImGui::DragFloat3("Pos", &obj.transform.position.x, 0.5f);
                ImGui::DragFloat3("Rot", &obj.transform.rotation.x, 1.0f);
                ImGui::DragFloat3("Scale", &obj.transform.scale.x, 0.05f, 0.01f, 100.0f);
            }

            ImGui::PopID();
        }

        if (remove_pending_ >= 0) {
            vulkan_context_.wait_idle();
            scene_.remove(static_cast<uint32_t>(remove_pending_));
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextColored({0.42f, 0.44f, 0.48f, 1.0f}, "[Esc] Toggle Cursor");
    }
    ImGui::End();
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

        imgui_layer_->begin_frame();
        draw_ui();
        imgui_layer_->end_frame();

        float aspect = static_cast<float>(window_.width()) /
                       static_cast<float>(std::max(window_.height(), 1u));
        auto view = camera_.view_matrix();
        auto projection = camera_.projection_matrix(aspect);

        vulkan_context_.draw_frame(view, projection, &scene_);
    }
    FJELL_CORE_INFO("Exiting main loop");
}

} // namespace fjell
