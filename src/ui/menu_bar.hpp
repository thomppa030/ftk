#pragma once

#include "core/delegate.hpp"
#include "ui/file_browser.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fjell {

class GpuCore;
class Project;
class Scene;
class VulkanContext;
class ViewportManager;

/// Main menu bar panel. Follows the same Delegate pattern as HierarchyPanel
/// and ContentBrowser — owns its popup state, emits actions for Engine to handle.
class MenuBar {
public:
    void draw();

    // ── Per-frame state (set by Engine before draw) ─────────────────────
    void set_project(Project* p) { project_ = p; }
    void set_playing(bool p) { playing_ = p; }
    void set_building(bool b) { building_ = b; }
    void set_scene_path(const std::string& p) { scene_path_ = &p; }
    void set_vulkan_context(VulkanContext* vk) { vk_ = vk; }
    void set_viewport_manager(ViewportManager* vm) { viewports_ = vm; }
    void set_gpu(GpuCore* gpu);

    /// Tick standalone file browser windows. Call from the main loop.
    void tick_browsers();

    [[nodiscard]] const std::string& active_layout() const { return active_layout_name_; }
    void set_active_layout(const std::string& name) { active_layout_name_ = name; }

    // ── Actions — Engine binds handlers ─────────────────────────────────
    Delegate<void()> on_save_scene;
    Delegate<void()> on_load_scene;
    Delegate<void(const std::filesystem::path&)> on_open_project;
    Delegate<void(const std::filesystem::path&, const std::string&)> on_create_project;
    Delegate<void()> on_close_project;
    Delegate<void()> on_build_scripts;
    Delegate<void()> on_start_play;
    Delegate<void()> on_stop_play;
    Delegate<void(const std::string&)> on_new_script;
    Delegate<void()> on_build_game;
    Delegate<void(const std::string&)> on_save_layout;
    Delegate<void(const std::string&)> on_load_layout;
    Delegate<void()> on_import_model;

private:
    void draw_file_menu();
    void draw_project_menu();
    void draw_view_menu();
    void draw_layout_menu();
    void draw_scripts_menu();
    void draw_play_button();
    void draw_popups();

    // Per-frame (set externally)
    Project* project_{nullptr};
    VulkanContext* vk_{nullptr};
    ViewportManager* viewports_{nullptr};
    const std::string* scene_path_{nullptr};
    bool playing_{false};
    bool building_{false};

    // Owned state
    FileBrowser open_browser_;
    FileBrowser new_browser_;
    std::vector<Connection> browser_connections_;
    bool show_new_script_popup_{false};
    char new_script_name_[128]{};
    bool show_save_layout_popup_{false};
    char layout_name_buf_[128]{};
    std::string active_layout_name_;
};

} // namespace fjell
