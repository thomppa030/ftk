#pragma once

#include "ui/editor_context.hpp"
#include "ui/viewport_manager.hpp"
#include "renderer/resources/material_asset.hpp"
#include "renderer/resources/resource_handle.hpp"
#include "scene/scene.hpp"

#include <string>
#include <vector>

namespace fjell {

class BindlessTextures;
class Clock;
class IconCache;
class Input;
class Material;
class PipelineCache;
class ProjectManager;
class ResourceManager;
class VulkanContext;

struct MaterialEditorDeps {
    VulkanContext* vulkan_context;
    ResourceManager* resource_manager;
    Input* input;
    Clock* clock;
    ProjectManager* project_mgr;
    IconCache* icon_cache;
};

class MaterialEditorContext final : public EditorContext {
public:
    explicit MaterialEditorContext(MaterialEditorDeps deps);

    [[nodiscard]] const char* name() const override { return name_.c_str(); }
    [[nodiscard]] const char* context_type() const override { return "material"; }
    [[nodiscard]] std::vector<std::string> docked_window_names() const override;
    [[nodiscard]] std::string asset_path() const override { return fjmat_path_; }
    bool save() override;

    void init() override;
    void shutdown() override;

    void draw(float dt) override;
    void setup_dockspace(ImGuiID main_area) override;
    bool handle_shortcuts() override;

    // Viewport
    [[nodiscard]] Scene* render_scene() override { return &preview_scene_; }
    [[nodiscard]] std::vector<ViewportRenderRequest> build_render_requests() override;
    void apply_resize(VulkanContext& vk) override;

    // Open a .fjmat from the content browser
    void open_material(const std::string& fjmat_path);
    void save_material();

    [[nodiscard]] const std::string& material_path() const { return fjmat_path_; }

private:
    void draw_params_panel();
    void draw_texture_panel();
    void draw_texture_slot(const char* slot_name);
    void setup_preview_scene();

    MaterialEditorDeps deps_;
    std::string name_{"Material"};

    // Asset being edited
    std::string fjmat_path_;
    std::string fjmat_abs_path_;
    MaterialAsset asset_;

    // Loaded material
    Material* material_{nullptr};
    MaterialHandle material_handle_;

    // Preview scene
    Scene preview_scene_;
    uint32_t preview_node_id_{0};

    // Viewport
    ViewportManager viewport_manager_;
};

} // namespace fjell
