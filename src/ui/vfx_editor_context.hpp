#pragma once

#include "ui/editor_context.hpp"
#include "ui/viewport_manager.hpp"
#include "scene/scene.hpp"

#include <string>
#include <vector>

namespace fjell {

class Clock;
class IconCache;
class Input;
class ProjectManager;
class ResourceManager;
class VulkanContext;
struct VfxEffect;
struct VfxEmitter;
struct VfxModule;

struct VfxEditorDeps {
    VulkanContext* vulkan_context;
    ResourceManager* resource_manager;
    Input* input;
    Clock* clock;
    ProjectManager* project_mgr;
    IconCache* icon_cache;
};

class VfxEditorContext final : public EditorContext {
public:
    explicit VfxEditorContext(VfxEditorDeps deps);

    [[nodiscard]] const char* name() const override { return name_.c_str(); }

    void init() override;
    void shutdown() override;

    void draw(float dt) override;
    void setup_dockspace(ImGuiID main_area) override;
    bool handle_shortcuts() override;

    // Viewport
    [[nodiscard]] Scene* render_scene() override { return &preview_scene_; }
    [[nodiscard]] std::vector<ViewportRenderRequest> build_render_requests() override;
    void apply_resize(VulkanContext& vk) override;
    [[nodiscard]] bool any_viewport_hovered() const override;

    // Open a .fjvfx from the content browser
    void open_effect(const std::string& fjvfx_path);
    void save_effect();

    [[nodiscard]] const std::string& effect_path() const { return fjvfx_path_; }

private:
    void draw_emitter_list();
    void draw_module_stack();
    void draw_module_palette();
    void setup_preview();

    void draw_module_entry(VfxModule* mod, int index,
                           std::vector<std::unique_ptr<VfxModule>>& stack,
                           const char* stack_label);

    VfxEditorDeps deps_;
    std::string name_{"VFX"};

    // Asset
    std::string fjvfx_path_;
    std::string fjvfx_abs_path_;
    std::shared_ptr<VfxEffect> effect_;

    // Preview
    Scene preview_scene_;
    uint32_t preview_node_id_{0};
    ViewportManager viewport_manager_;

    // Selection
    int selected_emitter_{0};

    // Playback
    bool playing_{true};
    float time_scale_{1.0f};
};

} // namespace fjell
