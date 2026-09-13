#pragma once

#include "animation/blend_space.hpp"
#include "ui/editor_context.hpp"
#include "ui/viewport_manager.hpp"
#include "core/delegate.hpp"
#include "scene/scene.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fjell {

class AnimationClip;
class AnimationSubsystem;
class Clock;
class IconCache;
class Input;
class ProjectManager;
class ResourceManager;
class Skeleton;
class VulkanContext;

struct BlendSpace2DEditorDeps {
    VulkanContext* vulkan_context;
    ResourceManager* resource_manager;
    Input* input;
    Clock* clock;
    AnimationSubsystem* animation_subsystem;
    ProjectManager* project_mgr;
    IconCache* icon_cache;
};

class BlendSpace2DEditorContext final : public EditorContext {
public:
    explicit BlendSpace2DEditorContext(BlendSpace2DEditorDeps deps);

    [[nodiscard]] const char* name() const override { return name_.c_str(); }
    [[nodiscard]] const char* context_type() const override { return "blend2d"; }
    [[nodiscard]] std::vector<std::string> docked_window_names() const override;
    [[nodiscard]] std::string asset_path() const override { return asset_path_; }

    void init() override;
    void shutdown() override;

    void draw(float dt) override;
    void setup_dockspace(ImGuiID main_area) override;
    bool handle_shortcuts() override;
    bool save() override;

    // Viewport
    [[nodiscard]] Scene* render_scene() override { return &preview_scene_; }
    [[nodiscard]] std::vector<ViewportRenderRequest> build_render_requests() override;
    void apply_resize(VulkanContext& vk) override;

    void open(const std::string& path);

private:
    void draw_canvas();
    void draw_point_list();
    void draw_properties();
    void draw_clip_list();
    void update_preview(float dt);
    void reload_clips();
    void scan_available_clips();
    void load_skeleton(const std::string& skel_path);
    void setup_preview_state_machine();

    BlendSpace2DEditorDeps deps_;
    std::string name_{"Blend Space 2D"};
    std::string asset_path_;

    BlendSpace2D blend_space_;

    // Preview
    Scene preview_scene_;
    uint32_t preview_node_id_{0};
    std::shared_ptr<Skeleton> skeleton_;
    std::vector<std::shared_ptr<AnimationClip>> loaded_clips_;

    // Available clips (from skeleton directory)
    std::vector<std::string> available_clip_paths_;
    std::vector<std::string> available_clip_names_;

    // Canvas interaction
    glm::vec2 current_param_{0.0f, 0.0f};
    int selected_point_{-1};
    int hovered_point_{-1};
    int dragging_point_{-1};
    bool dragging_cursor_{false};

    ViewportManager viewport_manager_;
    std::vector<Connection> connections_;
};

} // namespace fjell
