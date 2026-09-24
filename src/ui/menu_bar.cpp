#include "ui/menu_bar.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"
#include "core/project.hpp"
#include "renderer/gpu/gpu_core.hpp"
#include "renderer/vulkan_context.hpp"
#include "ui/icon_cache.hpp"
#include "ui/kit/field.hpp"
#include "ui/kit/icons.hpp"
#include "ui/kit/menu.hpp"
#include "ui/kit/row.hpp"
#include "ui/viewport_manager.hpp"

#include <imgui.h>

#include <filesystem>

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
        ImGui::EndMainMenuBar();
    }
    draw_popups();
}

void MenuBar::draw_file_menu() {
    if (!ImGui::BeginMenu("File")) return;

    if (ui::menu_item({.icon = ui::icon::save, .label = "Save scene", .shortcut = "Ctrl S"})) {
        on_save_scene.broadcast();
    }
    const bool has_scene = scene_path_ && !scene_path_->empty();
    if (ui::menu_item({.icon = ui::icon::reload, .label = "Reload scene", .enabled = has_scene,
                       .disabled_reason = "The scene hasn't been saved yet"})) {
        on_load_scene.broadcast();
    }
    ImGui::Separator();
    if (ui::menu_item({.icon = ui::icon::import, .label = "Import model..."})) {
        on_import_model.broadcast();
    }
    ImGui::EndMenu();
}

void MenuBar::draw_project_menu() {
    if (!ImGui::BeginMenu("Project")) return;

    if (ui::menu_item({.icon = ui::icon::new_file, .label = "New project..."})) {
        new_browser_.open("New Project", FileBrowser::Mode::select_location);
    }
    if (ui::menu_item({.icon = ui::icon::open_file, .label = "Open project..."})) {
        open_browser_.open("Open Project", FileBrowser::Mode::select_directory);
    }
    if (project_) {
        if (ui::menu_item({.icon = ui::icon::save, .label = "Save project", .shortcut = "Ctrl Shift S"})) {
            on_save_project.broadcast();
        }
        if (ui::menu_item({.icon = ui::icon::close, .label = "Close project"})) {
            on_close_project.broadcast();
        }
        ImGui::Separator();
        if (ui::menu_item({.icon = ui::icon::settings, .label = "Settings"})) {
            on_open_settings.broadcast();
        }
        ImGui::Separator();
        if (ui::menu_item({.icon = ui::icon::build, .label = "Build game"})) {
            on_build_game.broadcast();
        }
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

    if (viewports_ && ui::menu_item({.icon = ui::icon::viewport, .label = "Add viewport"})) {
        viewports_->add_viewport();
    }
    ImGui::Separator();
    if (vk_) {
        auto& grid = vk_->grid_pass().settings();
        if (ui::menu_item({.label = "Show grid", .checked = grid.visible})) grid.visible = !grid.visible;
        if (ui::menu_item({.label = "Snap to grid", .checked = grid.snap_enabled})) {
            grid.snap_enabled = !grid.snap_enabled;
        }
        // A menu is sunken, so its fields are inset.
        ui::InsetFields inset;
        if (auto t = ui::PropertyTable("##grid")) {
            ui::row("Grid size", [&] {
                (void)ui::drag("##size", grid.cell_size, {.speed = 0.1f, .min = 0.1f, .max = 50.0f, .unit = ui::Unit::Metres});
            });
            ui::row("Rotation snap", [&] {
                (void)ui::drag("##rotation", grid.rotation_snap_degrees,
                               {.speed = 1.0f, .min = 1.0f, .max = 90.0f, .unit = ui::Unit::Degrees, .format = "%.0f"});
            });
            ui::row("Scale snap", [&] {
                (void)ui::drag("##scale", grid.scale_snap, {.speed = 0.01f, .min = 0.01f, .max = 1.0f});
            });
            ui::row("Grid opacity", [&] {
                (void)ui::slider("##opacity", grid.opacity, 0.0f, 1.0f);
            });
            ui::row("Grid fade", [&] {
                (void)ui::drag("##fade", grid.fade_distance, {.speed = 1.0f, .min = 10.0f, .max = 500.0f, .unit = ui::Unit::Metres});
            });
        }
    }
    ImGui::Separator();
    if (vk_) {
        const bool sdf = vk_->show_sdf_debug();
        if (ui::menu_item({.label = "SDF debug", .checked = sdf})) vk_->set_show_sdf_debug(!sdf);
    }
    if (show_kit_gallery_ != nullptr &&
        ui::menu_item({.label = "Kit gallery", .checked = *show_kit_gallery_})) {
        *show_kit_gallery_ = !*show_kit_gallery_;
    }
    ImGui::Separator();
    if (ui::menu_item({.icon = ui::icon::capture, .label = "Capture screenshot...", .shortcut = "F9"})) {
        on_open_capture.broadcast();
    }
    ImGui::EndMenu();
}

void MenuBar::draw_layout_menu() {
    if (!ImGui::BeginMenu("Layout")) return;

    // Save overwrites the layout in use.
    const bool has_active = !active_layout_name_.empty();
    if (ui::menu_item({.icon = ui::icon::save, .label = "Save layout", .enabled = has_active,
                       .disabled_reason = "No layout is in use: save it under a name first"})) {
        on_save_layout.broadcast(active_layout_name_);
    }
    if (ui::menu_item({.icon = ui::icon::save, .label = "Save layout as..."})) {
        show_save_layout_popup_ = true;
        std::memset(layout_name_buf_, 0, sizeof(layout_name_buf_));
    }
    ImGui::Separator();

    // The saved layouts, the one in use checked.
    auto layout_dir = project_ ? project_->root() / "layouts" : base_path::dir() / "layouts";
    if (std::filesystem::is_directory(layout_dir)) {
        for (const auto& entry : std::filesystem::directory_iterator(layout_dir)) {
            if (entry.path().extension() != ".ini") continue;
            auto name = entry.path().stem().string();
            if (ui::menu_item({.label = name.c_str(), .checked = name == active_layout_name_})) {
                on_load_layout.broadcast(name);
            }
        }
    }
    ImGui::EndMenu();
}

void MenuBar::draw_scripts_menu() {
    if (!project_ || !ImGui::BeginMenu("Scripts")) return;

    if (ui::menu_item({.icon = ui::icon::new_file, .label = "New script..."})) {
        show_new_script_popup_ = true;
        std::memset(new_script_name_, 0, sizeof(new_script_name_));
    }
    ImGui::Separator();
    if (ui::menu_item({.icon = ui::icon::reload, .label = building_ ? "Building..." : "Build and reload",
                       .enabled = !building_, .disabled_reason = "A build is running"})) {
        on_build_scripts.broadcast();
    }
    ImGui::EndMenu();
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

}

void MenuBar::set_gpu(GpuCore* gpu) {
    open_browser_.set_gpu(gpu);
    new_browser_.set_gpu(gpu);

    browser_connections_.push_back(
        open_browser_.on_selected.bind([this](const std::string& path) {
            on_open_project.broadcast(std::filesystem::path(path));
        }));
    browser_connections_.push_back(
        new_browser_.on_location_selected.bind([this](const std::string& path, const std::string& name) {
            on_create_project.broadcast(std::filesystem::path(path), name);
        }));
}

void MenuBar::tick_browsers() {
    if (open_browser_.is_open()) open_browser_.tick();
    if (new_browser_.is_open()) new_browser_.tick();
}

} // namespace fjell
