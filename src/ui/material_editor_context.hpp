#pragma once

#include "ui/engine_editor_context.hpp"
#include "ui/viewport_manager.hpp"
#include "renderer/resources/material_asset.hpp"
#include "renderer/resources/resource_handle.hpp"
#include "scene/scene.hpp"

#include <string>
#include <vector>

namespace ftk {
class IconCache;
} // namespace ftk

namespace fjell {

class AssetThumbnailRenderer;
class BindlessTextures;
class Clock;
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
    ftk::IconCache* icon_cache;
    AssetThumbnailRenderer* asset_thumbnails;
};

class MaterialEditorContext final : public EngineEditorContext {
public:
    explicit MaterialEditorContext(MaterialEditorDeps deps);

    [[nodiscard]] const char* name() const override { return name_.c_str(); }
    [[nodiscard]] const char* context_type() const override { return "material"; }
    [[nodiscard]] DockPreset dock_preset() const override;
    void draw_header_actions() override;
    [[nodiscard]] std::string asset_path() const override { return fjmat_path_; }
    [[nodiscard]] std::string document() const override;
    [[nodiscard]] ftk::Result<> write() override;
    [[nodiscard]] ftk::Result<> restore(const std::string& text) override;

    void init() override;

    void draw(float dt) override;
    bool handle_shortcuts() override;

    // Viewport
    [[nodiscard]] Scene* render_scene() override { return &preview_scene_; }
    [[nodiscard]] std::vector<ViewportRenderRequest> build_render_requests() override;
    void apply_resize(VulkanContext& vk) override;

    // Open a .fjmat from the content browser
    void open_material(const std::string& fjmat_path);

    [[nodiscard]] const std::string& material_path() const { return fjmat_path_; }

private:
    void draw_params_panel();
    void draw_texture_panel();
    void setup_preview_scene();
    /// The asset with its parameter values as the GPU holds them now: the
    /// parameter fields edit the GPU copy directly.
    [[nodiscard]] MaterialAsset current_asset() const;
    /// Saves the asset and loads the material again: the only way a texture
    /// change reaches the GPU material.
    void save_and_reload();

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
