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
    void set_building(bool b) { building_ = b; }
    void set_scene_path(const std::string& p) { scene_path_ = &p; }
    void set_vulkan_context(VulkanContext* vk) { vk_ = vk; }
    /// The flag View > Kit Gallery toggles; the engine draws the window.
    void set_kit_gallery_flag(bool* show) { show_kit_gallery_ = show; }
    void set_viewport_manager(ViewportManager* vm) { viewports_ = vm; }
    void set_gpu(GpuCore* gpu);

    /// Tick standalone file browser windows. Call from the main loop.
    void tick_browsers();

    [[nodiscard]] const std::string& active_layout() const { return active_layout_name_; }
    void set_active_layout(const std::string& name) { active_layout_name_ = name; }

    // ── Actions — Engine binds handlers ─────────────────────────────────
    Delegate<void()> on_save_scene;
    Delegate<void()> on_load_scene;
    Delegate<void()> on_save_project;
    Delegate<void()> on_open_settings;
    Delegate<void(const std::filesystem::path&)> on_open_project;
    Delegate<void(const std::filesystem::path&, const std::string&)> on_create_project;
    Delegate<void()> on_close_project;
    Delegate<void()> on_build_scripts;
    Delegate<void(const std::string&)> on_new_script;
    Delegate<void()> on_build_game;
    Delegate<void(const std::string&)> on_save_layout;
    Delegate<void(const std::string&)> on_load_layout;
    Delegate<void()> on_import_model;
    Delegate<void()> on_open_capture;

private:
    void draw_file_menu();
    void draw_project_menu();
    void draw_view_menu();
    void draw_layout_menu();
    void draw_scripts_menu();
    void draw_popups();

    // Per-frame (set externally)
    Project* project_{nullptr};
    VulkanContext* vk_{nullptr};
    bool* show_kit_gallery_{nullptr};
    ViewportManager* viewports_{nullptr};
    const std::string* scene_path_{nullptr};
    bool building_{false};

    // Owned state
    FileBrowser open_browser_;
    FileBrowser new_browser_;
    std::vector<Connection> browser_connections_;
    bool show_new_script_popup_{false};
    std::string new_script_name_;
    bool show_save_layout_popup_{false};
    std::string layout_name_;
    std::string active_layout_name_;
};

} // namespace fjell
