#pragma once

#include "ui/editor_context.hpp"
#include "ui/timeline_widget.hpp"
#include "ui/viewport_manager.hpp"
#include "core/delegate.hpp"
#include "scene/scene.hpp"

#include <glm/glm.hpp>

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

struct AnimationEditorDeps {
    VulkanContext* vulkan_context;
    ResourceManager* resource_manager;
    Input* input;
    Clock* clock;
    AnimationSubsystem* animation_subsystem;
    ProjectManager* project_mgr;
    IconCache* icon_cache;
};

class AnimationEditorContext final : public EditorContext {
public:
    explicit AnimationEditorContext(AnimationEditorDeps deps);

    [[nodiscard]] const char* name() const override { return name_.c_str(); }
    [[nodiscard]] std::string asset_path() const override { return skeleton_path_; }

    void init() override;
    void shutdown() override;
    void on_enter() override;

    void draw(float dt) override;
    void setup_dockspace(ImGuiID main_area) override;
    bool handle_shortcuts() override;

    // Viewport
    [[nodiscard]] Scene* render_scene() override { return &preview_scene_; }
    [[nodiscard]] std::vector<ViewportRenderRequest> build_render_requests() override;
    void apply_resize(VulkanContext& vk) override;
    [[nodiscard]] bool any_viewport_hovered() const override;

    // Open from asset path
    void open_skeleton(const std::string& skel_path);
    void open_clip(const std::string& anim_path);

    [[nodiscard]] const std::string& skeleton_path() const { return skeleton_path_; }

private:
    void draw_bone_tree();
    void draw_clip_list();
    void draw_properties();
    void scan_clips();
    void load_clip(int index);
    void update_preview(float dt);

    AnimationEditorDeps deps_;
    std::string name_{"Animation"};

    // Asset being edited
    std::string skeleton_path_;
    std::string current_clip_path_;

    // Preview scene (isolated from main scene)
    Scene preview_scene_;
    uint32_t preview_node_id_{0};

    // Loaded data
    std::shared_ptr<Skeleton> skeleton_;
    std::vector<std::string> available_clip_paths_;
    std::vector<std::string> available_clip_names_;
    std::shared_ptr<AnimationClip> current_clip_;

    // Playback
    int selected_bone_{-1};
    int selected_clip_{-1};

    // Viewport
    ViewportManager viewport_manager_;
    TimelineWidget timeline_;

    // Delegate connections
    std::vector<Connection> connections_;
};

} // namespace fjell
