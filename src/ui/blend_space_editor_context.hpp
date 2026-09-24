#pragma once

#include "animation/blend_space.hpp"
#include "scene/scene.hpp"
#include "ui/editor_context.hpp"
#include "ui/viewport_manager.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
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

struct BlendSpaceEditorDeps {
    VulkanContext* vulkan_context;
    ResourceManager* resource_manager;
    Input* input;
    Clock* clock;
    AnimationSubsystem* animation_subsystem;
    ProjectManager* project_mgr;
    IconCache* icon_cache;
};

/// Edits a blend space, a `.fjblend1D` or a `.fjblend2D`: clips placed at
/// positions along one parameter or over two, blended by where the
/// parameters are. The two kinds share everything but the canvas, which is a
/// line for one parameter and a plane, triangulated, for two. The preview
/// plays the blend at the canvas's cursor on the skeleton the space is for.
class BlendSpaceEditorContext final : public EditorContext {
public:
    explicit BlendSpaceEditorContext(BlendSpaceEditorDeps deps);

    [[nodiscard]] const char* name() const override { return name_.c_str(); }
    [[nodiscard]] const char* context_type() const override { return "blend_space"; }
    [[nodiscard]] DockPreset dock_preset() const override;
    [[nodiscard]] std::string asset_path() const override { return asset_path_; }

    void init() override;
    void draw(float dt) override;
    bool handle_shortcuts() override { return false; }
    [[nodiscard]] std::string document() const override;
    [[nodiscard]] Result<> write() override;
    [[nodiscard]] Result<> restore(const std::string& text) override;

    [[nodiscard]] Scene* render_scene() override { return &preview_scene_; }
    [[nodiscard]] std::vector<ViewportRenderRequest> build_render_requests() override;
    void apply_resize(VulkanContext& vk) override;

    /// Opens a `.fjblend1D` or `.fjblend2D` by its project-relative path.
    void open(const std::string& path);

private:
    [[nodiscard]] bool two_d() const { return std::holds_alternative<BlendSpace2D>(blend_space_); }

    // The points, whichever kind the space is. A 1D position is x; a 1D
    // space keeps its points in order, so an edit returns where the point
    // went.
    [[nodiscard]] std::size_t point_count() const;
    [[nodiscard]] const std::string& point_clip(std::size_t i) const;
    [[nodiscard]] glm::vec2 point_position(std::size_t i) const;
    int set_point(std::size_t i, const std::string& clip, glm::vec2 position);
    int add_point(const std::string& clip, glm::vec2 position);
    void remove_point(std::size_t i);
    /// The clips the cursor blends and by how much, largest first.
    [[nodiscard]] std::vector<std::pair<std::size_t, float>> blend_at_cursor() const;

    void draw_canvas_panel();
    void draw_canvas_1d(ImVec2 origin, ImVec2 size);
    void draw_canvas_2d(ImVec2 origin, ImVec2 size);
    /// What happens under the canvas's mouse, both kinds alike: picking,
    /// dragging, double-click to add, a right-click menu, Del, and a clip
    /// dropped on. `to_screen` and `from_screen` map positions both ways.
    template <typename ToScreen, typename FromScreen>
    void canvas_input(ImVec2 origin, ImVec2 size, ToScreen&& to_screen, FromScreen&& from_screen);
    void draw_points_panel();
    void draw_clips_panel();
    void draw_properties_panel();

    void points_changed();
    void update_preview(float dt);
    void reload_clips();
    void scan_available_clips();
    void load_skeleton(const std::string& skel_path);
    void setup_preview_state_machine();

    BlendSpaceEditorDeps deps_;
    std::string name_{"Blend space"};
    std::string asset_path_;
    std::variant<BlendSpace1D, BlendSpace2D> blend_space_;

    // Preview
    Scene preview_scene_;
    uint32_t preview_node_id_{0};
    std::shared_ptr<Skeleton> skeleton_;
    // Loaded clips, parallel to the points.
    std::vector<std::shared_ptr<AnimationClip>> loaded_clips_;

    // Clips next to the skeleton, offered to drag onto the canvas.
    std::vector<std::string> available_clip_paths_;
    std::vector<std::string> available_clip_names_;

    // Canvas
    glm::vec2 cursor_{0.0f, 0.0f};
    int selected_point_{-1};
    int hovered_point_{-1};
    int dragging_point_{-1};
    bool dragging_cursor_{false};
    glm::vec2 menu_position_{0.0f, 0.0f};

    ViewportManager viewport_manager_;
};

} // namespace fjell
