#pragma once

namespace fjell {

class VulkanContext;
class ProjectManager;

// Project → Settings panel. Owns no state beyond pointers — edits the
// active project's ProjectConfig::render directly and applies the
// change live through VulkanContext::set_render_settings(). Values
// persist on Save Project (Ctrl+Shift+S) via the
// snapshot_render_settings() path in engine.cpp.
//
// Render is the only section for now; future project-level settings
// (input, scripting options, window defaults) should land here as
// additional sections rather than their own panels.
class ProjectSettingsPanel {
public:
    void init(VulkanContext* vk, ProjectManager* projects);
    void draw(const char* title);

    [[nodiscard]] bool visible() const { return visible_; }
    void set_visible(bool v) { visible_ = v; }
    void toggle() { visible_ = !visible_; }

private:
    void draw_render_section();
    void draw_debug_section();
    void draw_ui_section();
    void draw_environment_section();

    VulkanContext* vk_{nullptr};
    ProjectManager* projects_{nullptr};
    bool visible_{false};
};

} // namespace fjell
