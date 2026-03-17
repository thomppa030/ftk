#pragma once

#include "ui/file_browser.hpp"
#include "hub/recent_projects.hpp"

#include <string>

namespace fjell::hub {

class HubUI {
public:
    HubUI();

    // Draw the current frame. Returns a project path to launch, or empty.
    [[nodiscard]] std::string draw(float window_w, float window_h);

    [[nodiscard]] bool is_splash_active() const { return splash_active_; }
    [[nodiscard]] float splash_elapsed() const { return splash_timer_; }
    void update_splash(float dt);

    [[nodiscard]] const std::string& launch_path() const { return launch_path_; }

    RecentProjects& recent() { return recent_; }

private:
    void draw_logo(float cx, float y, float scale);
    void draw_hub(float window_w, float window_h);
    void draw_splash(float window_w, float window_h);
    void setup_style();

    void launch_project(const std::string& path, const std::string& name);

    RecentProjects recent_;
    bool splash_active_{false};
    float splash_timer_{0.0f};
    std::string launch_path_;
    std::string launch_name_;

    // File browsers
    FileBrowser open_browser_{FileBrowser::Mode::select_directory};
    FileBrowser new_browser_{FileBrowser::Mode::select_location};

    bool style_set_{false};
};

} // namespace fjell::hub
