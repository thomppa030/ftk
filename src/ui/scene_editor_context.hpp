#pragma once

#include "ui/editor_context.hpp"
#include "ui/hierarchy_panel.hpp"
#include "ui/net_panel.hpp"
#include "ui/node_editor_panel.hpp"
#include "ui/properties_panel.hpp"
#include "ui/viewport_manager.hpp"
#include "core/delegate.hpp"
#include "scene/transform.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace fjell {

class AssetLoader;
class Clock;
class GameUi;
class IconCache;
class Input;
class ProjectManager;
class ResourceManager;
class ScriptSubsystem;
class ThreadPool;
class ContentBrowser;
class FileBrowser;
class NetManager;
class CameraSubsystem;
class PlayController;
class ScreenDebugQueue;
class VulkanContext;
class Window;
struct DirectionalLight;

struct SceneEditorDeps {
    Scene* scene;
    VulkanContext* vulkan_context;
    ResourceManager* resource_manager;
    AssetLoader* asset_loader;
    Input* input;
    Clock* clock;
    ScriptSubsystem* script_subsystem;
    ProjectManager* project_mgr;
    IconCache* icon_cache;
    DirectionalLight* light;
    GameUi* game_ui;
    ThreadPool* thread_pool;
    Window* window;
    ContentBrowser* content_browser{nullptr};
    FileBrowser* file_browser{nullptr};
    NetManager* net_manager{nullptr};
    CameraSubsystem* camera_subsystem{nullptr};
    ScreenDebugQueue* screen_debug_queue{nullptr};
    PlayController* play{nullptr};
};

class SceneEditorContext final : public EditorContext {
public:
    explicit SceneEditorContext(SceneEditorDeps deps);

    [[nodiscard]] const char* name() const override { return "Scene"; }
    [[nodiscard]] const char* context_type() const override { return "scene"; }
    [[nodiscard]] std::vector<std::string> docked_window_names() const override;

    void init() override;
    void shutdown() override;

    void draw(float dt) override;
    void setup_dockspace(ImGuiID main_area) override;
    bool handle_shortcuts() override;

    // Scene to render
    [[nodiscard]] Scene* render_scene() override { return deps_.scene; }

    void draw_play_overlay() override;

    // Viewport render requests
    [[nodiscard]] std::vector<ViewportRenderRequest> build_render_requests() override;
    void apply_resize(VulkanContext& vk) override;
    void render_previews(VulkanContext& vk, Scene* scene, float dt,
                          const DirectionalLight& light, ThreadPool& pool) override;

    // Scene mutation (called from Engine delegate bindings)
    void add_model(const std::string& path);
    void add_skeletal_model(const std::string& mesh_path,
                            const std::string& skel_path,
                            const std::string& anim_path = {});
    void add_primitive(const std::string& type);

    // Viewport access for Engine (play mode camera override, source mask, etc.)
    ViewportManager& viewport_manager() { return viewport_manager_; }
    void request_focus_viewport() { focus_viewport_frames_ = 2; }

    // Selection access
    [[nodiscard]] uint32_t selected_id() const { return selected_id_; }
    [[nodiscard]] uint32_t* selected_id_ptr() { return &selected_id_; }
    void set_selected_id(uint32_t id) { selected_id_ = id; }

private:
    void duplicate_selection();
    void delete_selection();
    void snap_selection_to_floor();
    void draw_picking(ViewportPanel& vp);
    void draw_gizmo(ViewportPanel& vp);
    void draw_play_toolbar();
    void draw_spline_visuals();

    // Screen-space debug text submitted by scripts (fjell::Debug::screen /
    // print / watch / text_3d). Drawn into the rect the scene image occupies,
    // so the same code serves the fullscreen play view and the editor panel.
    // The caller supplies the draw list of the window it is drawing inside,
    // so the text clips to that window like every other viewport overlay.
    void draw_screen_debug(ImDrawList* dl, const glm::vec2& origin,
                           const glm::vec2& size);

    // Gizmo drag state
    bool gizmo_dragging_{false};
    glm::mat4 gizmo_matrix_{1.0f};
    glm::mat4 gizmo_start_world_{1.0f};
    Transform gizmo_start_transform_{};

    // Dependencies (non-owning)
    SceneEditorDeps deps_;

    // Context-owned panels
    HierarchyPanel hierarchy_panel_;
    PropertiesPanel properties_panel_;
    NodeEditorPanel node_editor_panel_;
    ViewportManager viewport_manager_;
    NetPanel net_panel_;

    // Context-owned state
    uint32_t selected_id_{0};
    int gizmo_op_{0};
    int focus_viewport_frames_{0}; // countdown: focus viewport after N frames

    // Play-mode FPS overlay (frames counted locally, updated every 0.5s).
    // Kept out of the game UI context so the HUD layout never rebuilds for it.
    double play_fps_accum_s_{0.0};
    uint32_t play_fps_count_{0};
    float play_fps_value_{0.0f};

    // Delegate connections (context-level bindings)
    std::vector<Connection> connections_;

    // Camera preview (PIP overlay when CameraNode selected)
    std::unique_ptr<OffscreenPass> camera_preview_target_;
    std::unique_ptr<PostProcessResources> camera_preview_pp_;
    // Private global descriptor set so the preview's view-dependent image
    // bindings never mutate the shared per-frame set the main CB binds.
    VkDescriptorSet camera_preview_global_set_{VK_NULL_HANDLE};
    uint32_t camera_preview_node_id_{0};  // currently previewing this camera
    int camera_preview_skip_frames_{0};   // skip N frames after creation
    // Cached editor_preview.fjgraph pointer. Set on first need_preview;
    // owned by VulkanContext::graph_cache_, so the pointer is stable.
    const struct FjGraph* camera_preview_graph_{nullptr};
};

} // namespace fjell
