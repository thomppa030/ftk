#include "ui/menu_bar.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"
#include "core/project.hpp"
#include "renderer/vulkan_context.hpp"
#include "ui/icon_cache.hpp"
#include "ui/viewport_manager.hpp"

#include <imgui.h>

#include <cstring>
#include <filesystem>

namespace fjell {

void MenuBar::draw() {
    if (ImGui::BeginMainMenuBar()) {
        draw_file_menu();
        draw_project_menu();
        draw_view_menu();
        draw_layout_menu();
        draw_scripts_menu();
        draw_play_button();
        ImGui::EndMainMenuBar();
    }
    draw_popups();
}

void MenuBar::draw_file_menu() {
    if (!ImGui::BeginMenu("File")) return;

    if (ImGui::MenuItem("Save Scene")) {
        on_save_scene.broadcast();
    }
    if (ImGui::MenuItem("Load Scene") && scene_path_ && !scene_path_->empty()) {
        on_load_scene.broadcast();
    }
    ImGui::EndMenu();
}

void MenuBar::draw_project_menu() {
    if (!ImGui::BeginMenu("Project")) return;

    if (ImGui::MenuItem("New Project...")) {
        new_browser_.open("New Project");
    }
    if (ImGui::MenuItem("Open Project...")) {
        open_browser_.open("Open Project");
    }
    if (project_ && ImGui::MenuItem("Close Project")) {
        on_close_project.broadcast();
    }
    ImGui::Separator();
    if (project_ && ImGui::MenuItem("Build Game")) {
        on_build_game.broadcast();
    }
    ImGui::Separator();
    if (project_) {
        ImGui::TextDisabled("Project: %s", project_->config().name.c_str());
        ImGui::TextDisabled("Root: %s", project_->root().c_str());
    } else {
        ImGui::TextDisabled("No project open");
    }
    ImGui::EndMenu();
}

void MenuBar::draw_view_menu() {
    if (!ImGui::BeginMenu("View")) return;

    if (viewports_ && ImGui::MenuItem("Add Viewport")) {
        viewports_->add_viewport();
    }
    ImGui::Separator();
    if (vk_) {
        auto& grid = vk_->grid_pass().settings();
        ImGui::Checkbox("Show Grid", &grid.visible);
        ImGui::Checkbox("Snap to Grid", &grid.snap_enabled);
        ImGui::DragFloat("Grid Size", &grid.cell_size, 0.1f, 0.1f, 50.0f);
        ImGui::DragFloat("Grid Opacity", &grid.opacity, 0.01f, 0.0f, 1.0f);
        ImGui::DragFloat("Grid Fade", &grid.fade_distance, 1.0f, 10.0f, 500.0f);
    }
    ImGui::EndMenu();
}

void MenuBar::draw_layout_menu() {
    if (!ImGui::BeginMenu("Layout")) return;

    // Save Layout — overwrite current active layout
    bool has_active = !active_layout_name_.empty();
    if (!has_active) ImGui::BeginDisabled();
    if (ImGui::MenuItem("Save Layout")) {
        on_save_layout.broadcast(active_layout_name_);
    }
    if (!has_active) ImGui::EndDisabled();

    // Save Layout As
    if (ImGui::MenuItem("Save Layout As...")) {
        show_save_layout_popup_ = true;
        std::memset(layout_name_buf_, 0, sizeof(layout_name_buf_));
    }
    ImGui::Separator();

    // List saved layouts
    auto layout_dir = project_ ? project_->root() / "layouts" : base_path::dir() / "layouts";
    if (std::filesystem::is_directory(layout_dir)) {
        for (const auto& entry : std::filesystem::directory_iterator(layout_dir)) {
            if (entry.path().extension() != ".ini") continue;
            auto name = entry.path().stem().string();
            bool is_active = (name == active_layout_name_);
            if (ImGui::MenuItem(name.c_str(), nullptr, is_active)) {
                on_load_layout.broadcast(name);
            }
        }
    }
    ImGui::EndMenu();
}

void MenuBar::draw_scripts_menu() {
    if (!project_ || !ImGui::BeginMenu("Scripts")) return;

    if (ImGui::MenuItem("New Script...")) {
        show_new_script_popup_ = true;
        std::memset(new_script_name_, 0, sizeof(new_script_name_));
    }
    ImGui::Separator();
    if (building_) {
        ImGui::MenuItem("Building...", nullptr, false, false);
    } else if (ImGui::MenuItem("Build & Reload")) {
        on_build_scripts.broadcast();
    }
    ImGui::EndMenu();
}

void MenuBar::draw_play_button() {
    // Play controls moved to viewport toolbar
}

void MenuBar::draw_popups() {
    // Save Layout As popup
    if (show_save_layout_popup_) {
        ImGui::OpenPopup("Save Layout As");
        show_save_layout_popup_ = false;
    }
    if (ImGui::BeginPopupModal("Save Layout As", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Layout Name:");
        ImGui::SetNextItemWidth(250.0f);
        bool enter = ImGui::InputText("##LayoutName", layout_name_buf_, sizeof(layout_name_buf_),
                                       ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1);

        bool valid = std::strlen(layout_name_buf_) > 0;
        if (!valid) ImGui::BeginDisabled();
        bool save = ImGui::Button("Save", {120, 0}) || (enter && valid);
        if (!valid) ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {120, 0})) ImGui::CloseCurrentPopup();

        if (save) {
            on_save_layout.broadcast(std::string(layout_name_buf_));
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // New Script popup
    if (show_new_script_popup_) {
        ImGui::OpenPopup("New Script");
        show_new_script_popup_ = false;
    }
    if (ImGui::BeginPopupModal("New Script", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Class Name:");
        ImGui::SetNextItemWidth(300.0f);
        bool enter = ImGui::InputText("##ClassName", new_script_name_, sizeof(new_script_name_),
                                       ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1);

        bool valid = std::strlen(new_script_name_) > 0;
        if (!valid) ImGui::BeginDisabled();
        bool create = ImGui::Button("Create", {140, 0}) || (enter && valid);
        if (!valid) ImGui::EndDisabled();
        ImGui::SameLine();
        bool cancel = ImGui::Button("Cancel", {140, 0});

        if (create) {
            on_new_script.broadcast(std::string(new_script_name_));
            ImGui::CloseCurrentPopup();
        }
        if (cancel) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // File browser popups
    if (open_browser_.draw()) {
        on_open_project.broadcast(open_browser_.selected_path());
    }

    if (new_browser_.draw()) {
        on_create_project.broadcast(new_browser_.selected_path(), new_browser_.project_name());
    }
}

} // namespace fjell
