#include "ui/menu_bar.hpp"
#include "core/base_path.hpp"
#include "ftk/base/log.hpp"
#include "core/project.hpp"
#include "renderer/vulkan_context.hpp"
#include "ui/engine_file_browser.hpp"
#include "ftk/ui/kit/dialog.hpp"
#include "ftk/ui/kit/field.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/menu.hpp"
#include "ftk/ui/kit/row.hpp"
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

    if (ftk::ui::menu_item({.icon = ftk::ui::icon::save, .label = "Save scene", .shortcut = "Ctrl S"})) {
        on_save_scene.broadcast();
    }
    const bool has_scene = scene_path_ && !scene_path_->empty();
    if (ftk::ui::menu_item({.icon = ftk::ui::icon::reload, .label = "Reload scene", .enabled = has_scene,
                       .disabled_reason = "The scene hasn't been saved yet"})) {
        on_load_scene.broadcast();
    }
    ImGui::Separator();
    if (ftk::ui::menu_item({.icon = ftk::ui::icon::import, .label = "Import model..."})) {
        on_import_model.broadcast();
    }
    ImGui::EndMenu();
}

void MenuBar::draw_project_menu() {
    if (!ImGui::BeginMenu("Project")) return;

    if (ftk::ui::menu_item({.icon = ftk::ui::icon::new_file, .label = "New project..."})) {
        new_browser_.open("New project", ftk::FileBrowser::Mode::select_location, "Create");
    }
    if (ftk::ui::menu_item({.icon = ftk::ui::icon::open_file, .label = "Open project..."})) {
        open_browser_.open("Open project", ftk::FileBrowser::Mode::select_directory, "Open");
    }
    if (project_) {
        if (ftk::ui::menu_item({.icon = ftk::ui::icon::save, .label = "Save project", .shortcut = "Ctrl Shift S"})) {
            on_save_project.broadcast();
        }
        if (ftk::ui::menu_item({.icon = ftk::ui::icon::close, .label = "Close project"})) {
            on_close_project.broadcast();
        }
        ImGui::Separator();
        if (ftk::ui::menu_item({.icon = ftk::ui::icon::settings, .label = "Settings...", .shortcut = "Ctrl ,"})) {
            on_open_settings.broadcast();
        }
        ImGui::Separator();
        if (ftk::ui::menu_item({.icon = ftk::ui::icon::build, .label = "Build game"})) {
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

    if (viewports_ && ftk::ui::menu_item({.icon = ftk::ui::icon::viewport, .label = "Add viewport"})) {
        viewports_->add_viewport();
    }
    ImGui::Separator();
    if (vk_) {
        auto& grid = vk_->grid_pass().settings();
        if (ftk::ui::menu_item({.label = "Show grid", .checked = grid.visible})) grid.visible = !grid.visible;
        if (ftk::ui::menu_item({.label = "Snap to grid", .checked = grid.snap_enabled})) {
            grid.snap_enabled = !grid.snap_enabled;
        }
        // A menu is sunken, so its fields are inset.
        ftk::ui::InsetFields inset;
        if (auto t = ftk::ui::PropertyTable("##grid")) {
            ftk::ui::row("Grid size", [&] {
                (void)ftk::ui::drag("##size", grid.cell_size, {.speed = 0.1f, .min = 0.1f, .max = 50.0f, .unit = ftk::ui::Unit::Metres});
            });
            ftk::ui::row("Rotation snap", [&] {
                (void)ftk::ui::drag("##rotation", grid.rotation_snap_degrees,
                               {.speed = 1.0f, .min = 1.0f, .max = 90.0f, .unit = ftk::ui::Unit::Degrees, .format = "%.0f"});
            });
            ftk::ui::row("Scale snap", [&] {
                (void)ftk::ui::drag("##scale", grid.scale_snap, {.speed = 0.01f, .min = 0.01f, .max = 1.0f});
            });
            ftk::ui::row("Grid opacity", [&] {
                (void)ftk::ui::slider("##opacity", grid.opacity, 0.0f, 1.0f);
            });
            ftk::ui::row("Grid fade", [&] {
                (void)ftk::ui::drag("##fade", grid.fade_distance, {.speed = 1.0f, .min = 10.0f, .max = 500.0f, .unit = ftk::ui::Unit::Metres});
            });
        }
    }
    ImGui::Separator();
    if (show_sdf_debug_ != nullptr &&
        ftk::ui::menu_item({.label = "SDF debug", .checked = *show_sdf_debug_})) {
        *show_sdf_debug_ = !*show_sdf_debug_;
    }
    if (show_kit_gallery_ != nullptr &&
        ftk::ui::menu_item({.label = "Kit gallery", .checked = *show_kit_gallery_})) {
        *show_kit_gallery_ = !*show_kit_gallery_;
    }
    ImGui::Separator();
    if (ftk::ui::menu_item({.icon = ftk::ui::icon::capture, .label = "Capture screenshot...", .shortcut = "F9"})) {
        on_open_capture.broadcast();
    }
    ImGui::EndMenu();
}

void MenuBar::draw_layout_menu() {
    if (!ImGui::BeginMenu("Layout")) return;

    // Save overwrites the layout in use.
    const bool has_active = !active_layout_name_.empty();
    if (ftk::ui::menu_item({.icon = ftk::ui::icon::save, .label = "Save layout", .enabled = has_active,
                       .disabled_reason = "No layout is in use: save it under a name first"})) {
        on_save_layout.broadcast(active_layout_name_);
    }
    if (ftk::ui::menu_item({.icon = ftk::ui::icon::save, .label = "Save layout as..."})) {
        show_save_layout_popup_ = true;
        menu_anchor_ = {ImGui::GetWindowPos(), 0.0f};
        layout_name_.clear();
    }
    ImGui::Separator();

    // The saved layouts, the one in use checked.
    auto layout_dir = project_ ? project_->root() / "layouts" : base_path::dir() / "layouts";
    if (std::filesystem::is_directory(layout_dir)) {
        for (const auto& entry : std::filesystem::directory_iterator(layout_dir)) {
            if (entry.path().extension() != ".ini") continue;
            auto name = entry.path().stem().string();
            if (ftk::ui::menu_item({.label = name.c_str(), .checked = name == active_layout_name_})) {
                on_load_layout.broadcast(name);
            }
        }
    }
    ImGui::EndMenu();
}

void MenuBar::draw_scripts_menu() {
    if (!project_ || !ImGui::BeginMenu("Scripts")) return;

    if (ftk::ui::menu_item({.icon = ftk::ui::icon::new_file, .label = "New script..."})) {
        show_new_script_popup_ = true;
        menu_anchor_ = {ImGui::GetWindowPos(), 0.0f};
        new_script_name_.clear();
    }
    ImGui::Separator();
    if (ftk::ui::menu_item({.icon = ftk::ui::icon::reload, .label = building_ ? "Building..." : "Build and reload",
                       .enabled = !building_, .disabled_reason = "A build is running"})) {
        on_build_scripts.broadcast();
    }
    ImGui::EndMenu();
}

void MenuBar::draw_popups() {
    static constexpr const char* SAVE_LAYOUT = "##save_layout";
    static constexpr const char* NEW_SCRIPT = "##new_script";

    if (show_save_layout_popup_) {
        ftk::ui::open_dialog(SAVE_LAYOUT, menu_anchor_);
        show_save_layout_popup_ = false;
    }
    if (ftk::ui::prompt_dialog(SAVE_LAYOUT, {.title = "Save layout as", .hint = "Layout name", .confirm = "Save layout"},
                          layout_name_) == ftk::ui::DialogAnswer::Confirm) {
        on_save_layout.broadcast(layout_name_);
    }

    if (show_new_script_popup_) {
        ftk::ui::open_dialog(NEW_SCRIPT, menu_anchor_);
        show_new_script_popup_ = false;
    }
    if (ftk::ui::prompt_dialog(NEW_SCRIPT, {.title = "New script", .hint = "Class name", .confirm = "Create script"},
                          new_script_name_) == ftk::ui::DialogAnswer::Confirm) {
        on_new_script.broadcast(new_script_name_);
    }
}

void MenuBar::set_device(ftk::gpu::Device* device) {
    set_up_file_browser(open_browser_, device);
    set_up_file_browser(new_browser_, device);

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
