#pragma once

#include "ftk/app/file_browser.hpp"
#include "hub/recent_projects.hpp"

#include <string>

namespace fjell::hub {

class HubUI {
public:
    HubUI();

    /// Set the device the file browser windows draw with.
    void set_device(ftk::gpu::Device* device);

    // Draw the current frame. Returns a project path to launch, or empty.
    [[nodiscard]] std::string draw(float window_w, float window_h);

    /// Tick standalone file browser windows.
    void tick_browsers();

    [[nodiscard]] bool is_splash_active() const { return splash_active_; }
    [[nodiscard]] float splash_elapsed() const { return splash_timer_; }
    void update_splash(float dt);

    [[nodiscard]] const std::string& launch_path() const { return launch_path_; }

    /// Non-empty when the pending launch should scaffold a new project of this
    /// name rather than open an existing one.
    [[nodiscard]] const std::string& launch_create_name() const { return launch_create_name_; }

    RecentProjects& recent() { return recent_; }

private:
    void draw_hub(float window_w, float window_h);
    void draw_sidebar();
    void draw_projects();
    void draw_splash(float window_w, float window_h);
    void setup_style();

    void launch_project(const std::string& path, const std::string& name,
                        const std::string& create_name = {});

    RecentProjects recent_;
    std::string search_;
    bool splash_active_{false};
    float splash_timer_{0.0f};
    std::string launch_path_;
    std::string launch_name_;
    std::string launch_create_name_;

    // File browsers (standalone windows)
    ftk::FileBrowser open_browser_;
    ftk::FileBrowser new_browser_;
    std::vector<ftk::Connection> browser_connections_;

    bool style_set_{false};
};

} // namespace fjell::hub
