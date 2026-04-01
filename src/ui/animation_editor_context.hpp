#pragma once

#include "animation/animation_state_machine.hpp"
#include "ui/editor_context.hpp"
#include "ui/timeline_widget.hpp"
#include "ui/viewport_manager.hpp"
#include "core/delegate.hpp"
#include "scene/scene.hpp"

#include <imgui.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
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
    [[nodiscard]] const char* context_type() const override { return "animation"; }
    [[nodiscard]] std::vector<std::string> docked_window_names() const override;
    [[nodiscard]] std::string asset_path() const override {
        return animset_path_.empty() ? skeleton_path_ : animset_path_;
    }

    void init() override;
    void shutdown() override;
    void on_enter() override;

    void draw(float dt) override;
    void setup_dockspace(ImGuiID main_area) override;
    bool handle_shortcuts() override;
    bool save() override;

    // Viewport
    [[nodiscard]] Scene* render_scene() override { return &preview_scene_; }
    [[nodiscard]] std::vector<ViewportRenderRequest> build_render_requests() override;
    void apply_resize(VulkanContext& vk) override;
    [[nodiscard]] bool any_viewport_hovered() const override;

    // Open from asset path
    void open_skeleton(const std::string& skel_path);
    void open_clip(const std::string& anim_path);
    void open_animset(const std::string& animset_path);

    [[nodiscard]] const std::string& skeleton_path() const { return skeleton_path_; }

private:
    // Graph data for imnodes state machine editor
    struct AnimStateNode {
        int node_id;
        int input_pin_id;
        int output_pin_id;
        int state_index;
    };

    struct AnimTransitionLink {
        int link_id;
        int start_pin;
        int end_pin;
        int source_state;
        int target_state;
        int transition_index;
    };

    void draw_bone_tree();
    void draw_clip_list();
    void draw_properties();
    void draw_state_graph();
    void draw_parameters();
    void scan_clips();
    void load_clip(int index);
    void update_preview(float dt);
    void sync_markers_from_clip();

    int next_node_id() { return ++node_id_counter_; }
    void build_graph_from_state_machine();
    void sync_preview_state_machine();
    void load_animset_clips(const std::string& animset_resolved_path);

    AnimationEditorDeps deps_;
    std::string name_{"Animation"};

    // Asset being edited
    std::string skeleton_path_;
    std::string current_clip_path_;
    std::string animset_path_;

    // Preview scene (isolated from main scene)
    Scene preview_scene_;
    uint32_t preview_node_id_{0};

    // Loaded data
    std::shared_ptr<Skeleton> skeleton_;
    std::vector<std::string> available_clip_paths_;
    std::vector<std::string> available_clip_names_;
    std::shared_ptr<AnimationClip> current_clip_;

    // State machine (editor-owned copy)
    AnimationStateMachine state_machine_;

    // Playback
    int selected_bone_{-1};
    int selected_clip_{-1};
    int selected_event_index_{-1};

    // imnodes graph state
    void* imnodes_ctx_{nullptr};
    int node_id_counter_{0};
    std::vector<AnimStateNode> state_nodes_;
    std::vector<AnimTransitionLink> transition_links_;
    std::unordered_map<int, ImVec2> pending_positions_;
    int selected_state_node_{-1};
    int selected_link_{-1};

    // Parameter editing
    char new_param_name_[64]{};

    // Add Notify popup
    float pending_notify_time_{0.0f};
    bool open_add_notify_popup_{false};

    // Viewport
    ViewportManager viewport_manager_;
    TimelineWidget timeline_;

    // Delegate connections
    std::vector<Connection> connections_;
};

} // namespace fjell
