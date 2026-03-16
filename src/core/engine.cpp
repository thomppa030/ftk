#include "core/engine.hpp"
#include "core/log.hpp"
#include "renderer/material.hpp"
#include "renderer/model.hpp"
#include "renderer/render_stats.hpp"
#include "renderer/texture.hpp"

#include <imgui.h>
#include <ImGuizmo.h>
#include <GLFW/glfw3.h>
#include <glm/gtc/type_ptr.hpp>

#include <filesystem>

namespace fjell {

Engine::Engine(std::string_view title, uint32_t width, uint32_t height)
    : window_{title, width, height}
    , camera_{{0.0f, 1.0f, 0.0f}, 8.0f}
    , vulkan_context_{window_} {
    window_.set_input(&input_);
    input_.attach(window_.handle());

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

    resource_manager_->load_texture("assets/textures/test.png");

    scan_models();

    // Try loading saved scene, fall back to default
    if (std::filesystem::exists(SCENE_FILE)) {
        scene_ = Scene::load(SCENE_FILE, *resource_manager_);
    } else {
        create_default_scene();
    }

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

void Engine::create_default_scene() {
    vulkan_context_.wait_idle();
    auto* tex = resource_manager_->load_texture("assets/textures/test.png");
    auto* mat = resource_manager_->create_material("default", tex);

    auto* plane = resource_manager_->create_primitive("plane");
    scene_.add(plane, mat, {{0.0f, 0.0f, 0.0f}, {}, {10.0f, 1.0f, 10.0f}},
               "Ground", "primitive:plane");

    auto* cube = resource_manager_->create_primitive("cube");
    scene_.add(cube, mat, {{0.0f, 0.5f, 0.0f}},
               "Cube", "primitive:cube");

    auto* sphere = resource_manager_->create_primitive("sphere");
    scene_.add(sphere, mat, {{2.5f, 0.5f, 0.0f}},
               "Sphere", "primitive:sphere");

    FJELL_CORE_INFO("Default scene created");
}

void Engine::add_model_to_scene(const std::string& path) {
    vulkan_context_.wait_idle();

    auto* model = resource_manager_->load_model(path);
    auto* tex = resource_manager_->load_texture("assets/textures/test.png");
    auto* mat = resource_manager_->create_material("default", tex);
    std::string mat_name = "default";

    if (!model->textures().empty()) {
        mat_name = path + "_mat";
        mat = resource_manager_->create_material(
            mat_name,
            const_cast<Texture*>(model->textures()[0].get()));
    }

    auto filename = std::filesystem::path(path).stem().string();
    scene_.add(model, mat, {}, filename, path, mat_name);
    FJELL_CORE_INFO("Added model to scene: {}", path);
}

void Engine::add_primitive_to_scene(const std::string& type) {
    vulkan_context_.wait_idle();

    auto* model = resource_manager_->create_primitive(type);
    if (!model) return;

    auto* tex = resource_manager_->load_texture("assets/textures/test.png");
    auto* mat = resource_manager_->create_material("default", tex);

    std::string name = type;
    name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));

    scene_.add(model, mat, {}, name, "primitive:" + type);
    selected_ = scene_.object_count() - 1;
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

    frame_time_history_[frame_time_index_] = clock_.delta_time() * 1000.0f;
    frame_time_index_ = (frame_time_index_ + 1) % FRAME_TIME_HISTORY_SIZE;

    const auto& stats = vulkan_context_.render_stats();

    // Gizmo keyboard shortcuts (only when ImGui doesn't want keyboard)
    if (!ImGui::GetIO().WantCaptureKeyboard) {
        if (ImGui::IsKeyPressed(ImGuiKey_W)) gizmo_op_ = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_E)) gizmo_op_ = 1;
        if (ImGui::IsKeyPressed(ImGuiKey_R)) gizmo_op_ = 2;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) selected_ = -1;
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
        ImGui::Text("GPU: %s", vulkan_context_.gpu_name().c_str());
        ImGui::Text("FPS: %.0f", fps_);
        ImGui::Text("Frame: %.2f ms", frame_time_);
        ImGui::Text("Draw calls: %u", stats.draw_calls);
        ImGui::Text("Triangles: %u", stats.triangle_count);
        ImGui::Spacing();

        ImGui::PlotLines("##FrameTime", frame_time_history_.data(),
                         FRAME_TIME_HISTORY_SIZE, frame_time_index_,
                         "Frame ms", 0.0f, 33.3f, {0, 40});

        ImGui::Spacing();
        auto pos = camera_.position();
        ImGui::Text("Camera: %.1f, %.1f, %.1f", pos.x, pos.y, pos.z);
    }
    ImGui::End();

    // Scene panel — left side
    ImGui::SetNextWindowPos(
        {ImGui::GetMainViewport()->WorkPos.x + pad,
         ImGui::GetMainViewport()->WorkPos.y + pad},
        ImGuiCond_Always, {0.0f, 0.0f});
    ImGui::SetNextWindowSizeConstraints({240, 100}, {340, 700});
    ImGui::SetNextWindowBgAlpha(0.85f);

    ImGuiWindowFlags panel_flags =
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing;

    if (ImGui::Begin("Scene", nullptr, panel_flags)) {
        // Add primitives
        ImGui::TextColored({0.50f, 0.60f, 0.82f, 1.0f}, "Add Primitive");
        ImGui::Separator();
        if (ImGui::Button("Cube", {70, 0})) add_primitive_to_scene("cube");
        ImGui::SameLine();
        if (ImGui::Button("Plane", {70, 0})) add_primitive_to_scene("plane");
        ImGui::SameLine();
        if (ImGui::Button("Sphere", {70, 0})) add_primitive_to_scene("sphere");

        ImGui::Spacing();

        // Models from disk
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

            bool is_selected = (i == selected_);
            if (ImGui::Selectable(obj.name.c_str(), is_selected)) {
                selected_ = i;
            }

            ImGui::SameLine(ImGui::GetWindowWidth() - 30);
            if (ImGui::SmallButton("X")) {
                remove_pending_ = i;
            }

            if (is_selected) {
                ImGui::Indent();
                ImGui::DragFloat3("Pos", &obj.transform.position.x, 0.1f);
                ImGui::DragFloat3("Rot", &obj.transform.rotation.x, 1.0f);
                ImGui::DragFloat3("Scale", &obj.transform.scale.x, 0.05f, 0.01f, 100.0f);
                ImGui::Unindent();
            }

            ImGui::PopID();
        }

        if (remove_pending_ >= 0) {
            vulkan_context_.wait_idle();
            if (selected_ == remove_pending_) selected_ = -1;
            else if (selected_ > remove_pending_) selected_--;
            scene_.remove(static_cast<uint32_t>(remove_pending_));
        }

        ImGui::Spacing();
        ImGui::Separator();

        // Gizmo mode
        ImGui::TextColored({0.50f, 0.60f, 0.82f, 1.0f}, "Gizmo");
        ImGui::RadioButton("Move (W)", &gizmo_op_, 0); ImGui::SameLine();
        ImGui::RadioButton("Rot (E)", &gizmo_op_, 1); ImGui::SameLine();
        ImGui::RadioButton("Scl (R)", &gizmo_op_, 2);

        ImGui::Spacing();
        ImGui::Separator();

        // Save / Load
        if (ImGui::Button("Save Scene", {-1, 0})) {
            scene_.save(SCENE_FILE);
        }
        if (ImGui::Button("Load Scene", {-1, 0})) {
            vulkan_context_.wait_idle();
            scene_ = Scene::load(SCENE_FILE, *resource_manager_);
            selected_ = -1;
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextColored({0.42f, 0.44f, 0.48f, 1.0f}, "[MMB] Orbit  [Shift+MMB] Pan  [Scroll] Zoom");
    }
    ImGui::End();

    // Draw gizmo for selected object
    if (selected_ >= 0 && selected_ < scene_.object_count()) {
        auto& obj = scene_.objects()[selected_];

        float aspect = static_cast<float>(window_.width()) /
                       static_cast<float>(std::max(window_.height(), 1u));
        auto view = camera_.view_matrix();
        // ImGuizmo expects OpenGL projection (no Vulkan Y-flip)
        auto projection = glm::perspective(
            glm::radians(camera_.fov()), aspect, 0.1f, 1000.0f);

        ImGuizmo::SetRect(0, 0,
            static_cast<float>(window_.width()),
            static_cast<float>(window_.height()));
        ImGuizmo::SetOrthographic(false);

        glm::mat4 model_matrix = obj.transform.matrix();

        ImGuizmo::OPERATION op = ImGuizmo::TRANSLATE;
        if (gizmo_op_ == 1) op = ImGuizmo::ROTATE;
        if (gizmo_op_ == 2) op = ImGuizmo::SCALE;

        ImGuizmo::Manipulate(
            glm::value_ptr(view),
            glm::value_ptr(projection),
            op,
            ImGuizmo::LOCAL,
            glm::value_ptr(model_matrix));

        if (ImGuizmo::IsUsing()) {
            float translation[3], rotation[3], scale[3];
            ImGuizmo::DecomposeMatrixToComponents(
                glm::value_ptr(model_matrix), translation, rotation, scale);
            obj.transform.position = {translation[0], translation[1], translation[2]};
            obj.transform.rotation = {rotation[0], rotation[1], rotation[2]};
            obj.transform.scale = {scale[0], scale[1], scale[2]};
        }
    }
}

void Engine::run() {
    FJELL_CORE_INFO("Entering main loop");
    while (!window_.should_close()) {
        input_.update();
        window_.poll_events();
        clock_.tick();

#ifndef NDEBUG
        if (shader_watcher_.poll()) {
            vulkan_context_.reload_pipeline();
        }
#endif

        // Only orbit camera when not using gizmo
        if (!ImGuizmo::IsUsing()) {
            camera_.update(input_, clock_.delta_time());
        }

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
