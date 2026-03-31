#pragma once

#include "animation/blend_space.hpp"
#include "ui/editor_context.hpp"
#include "ui/timeline_widget.hpp"
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

struct BlendSpace1DEditorDeps {
    VulkanContext* vulkan_context;
    ResourceManager* resource_manager;
    Input* input;
    Clock* clock;
    AnimationSubsystem* animation_subsystem;
    ProjectManager* project_mgr;
    IconCache* icon_cache;
};

class BlendSpace1DEditorContext final : public EditorContext {
public:
    explicit BlendSpace1DEditorContext(BlendSpace1DEditorDeps deps);

    [[nodiscard]] const char* name() const override { return name_.c_str(); }
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
    [[nodiscard]] bool any_viewport_hovered() const override;

    void open(const std::string& path);

private:
    void draw_point_list();
    void draw_parameter_slider();
    void draw_properties();
    void update_preview(float dt);
    void reload_clips();

    BlendSpace1DEditorDeps deps_;
    std::string name_{"Blend Space 1D"};
    std::string asset_path_;

    BlendSpace1D blend_space_;

    // Preview
    Scene preview_scene_;
    uint32_t preview_node_id_{0};
    std::shared_ptr<Skeleton> skeleton_;

    // Loaded clips (parallel to blend_space_.points())
    std::vector<std::shared_ptr<AnimationClip>> loaded_clips_;

    // Parameter slider
    float current_param_{0.0f};

    // Viewport
    ViewportManager viewport_manager_;

    std::vector<Connection> connections_;
};

} // namespace fjell
