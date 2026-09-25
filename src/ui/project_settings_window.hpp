#pragma once

#include "core/delegate.hpp"

#include <array>
#include <string>

namespace fjell {

class VulkanContext;
class ProjectManager;

// Project > Settings (Ctrl ,): a floating window, its pages listed on the
// left. Edits the active project's ProjectConfig directly and applies
// render settings live through VulkanContext::set_render_settings(); the
// footer saves them to project.fjell or goes back to what it holds.
//
// A new kind of project setting (input, scripting, window defaults) is a
// new page here rather than a panel of its own.
class ProjectSettingsWindow {
public:
    void init(VulkanContext* vk, ProjectManager* projects);
    void draw();

    [[nodiscard]] bool visible() const { return visible_; }
    void toggle() { visible_ = !visible_; }

    /// Save pressed: the project is saved the way Save project saves it.
    Delegate<void()> on_save;

private:
    enum class Page { Rendering, GameUi, Environment, Simulation };
    static constexpr std::size_t PAGE_COUNT = 4;

    void draw_page(Page page);
    void draw_rendering();
    void draw_game_ui();
    void draw_environment();
    void draw_simulation();

    /// Whether any page's settings differ from what project.fjell holds.
    [[nodiscard]] bool unsaved() const;
    void revert();

    VulkanContext* vk_{nullptr};
    ProjectManager* projects_{nullptr};
    bool visible_{false};
    Page page_{Page::Rendering};

    // The search over every page, and how many settings it found on each
    // the last time the pages were drawn.
    std::string search_;
    std::array<int, PAGE_COUNT> found_{};
};

} // namespace fjell
