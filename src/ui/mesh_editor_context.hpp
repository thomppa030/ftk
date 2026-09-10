#pragma once

#include "ui/editor_context.hpp"
#include "ui/viewport_manager.hpp"
#include "renderer/resources/resource_handle.hpp"
#include "scene/scene.hpp"

#include <string>
#include <vector>

namespace fjell {

class Clock;
class IconCache;
class Input;
class Model;
class ProjectManager;
class ResourceManager;
class VulkanContext;

struct MeshEditorDeps {
    VulkanContext* vulkan_context;
    ResourceManager* resource_manager;
    Input* input;
    Clock* clock;
    ProjectManager* project_mgr;
    IconCache* icon_cache;
};

class MeshEditorContext final : public EditorContext {
public:
    explicit MeshEditorContext(MeshEditorDeps deps);

    [[nodiscard]] const char* name() const override { return name_.c_str(); }
    [[nodiscard]] const char* context_type() const override { return "mesh"; }
    [[nodiscard]] std::vector<std::string> docked_window_names() const override;
    [[nodiscard]] std::string asset_path() const override { return fjmesh_path_; }

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

    // Open a .fjmesh from the content browser
    void open_mesh(const std::string& fjmesh_path);

    [[nodiscard]] const std::string& mesh_path() const { return fjmesh_path_; }

private:
    void draw_info_panel();
    void setup_preview_scene();
    void frame_camera();

    MeshEditorDeps deps_;
    std::string name_{"Mesh"};

    // Asset being inspected
    std::string fjmesh_path_;
    ModelHandle model_handle_;
    Model* model_{nullptr};

    // Preview scene
    Scene preview_scene_;
    uint32_t preview_node_id_{0};

    // Viewport
    ViewportManager viewport_manager_;
};

} // namespace fjell
