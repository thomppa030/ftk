#pragma once

#include "ui/editor_context.hpp"
#include "ui/game_ui/fjfont.hpp"

#include <memory>
#include <string>
#include <vector>

namespace fjell {

class AssetThumbnailRenderer;
class GameUi;
class OffscreenPass;
class ProjectManager;
class ResourceManager;
class VulkanContext;

struct FontEditorDeps {
    VulkanContext* vulkan_context;
    ResourceManager* resource_manager;
    ProjectManager* project_mgr;
    AssetThumbnailRenderer* asset_thumbnails;
};

/// Edits a `.fjfont` recipe: the font file, the charset, the cell size and
/// distance range, whether kerning is baked. Every committed field writes
/// the recipe and the font library rebuilds the atlas, so the sample line
/// and the atlas image show the result a moment later. Edits are undoable.
class FontEditorContext final : public EditorContext {
public:
    explicit FontEditorContext(FontEditorDeps deps);

    [[nodiscard]] const char* name() const override { return name_.c_str(); }
    [[nodiscard]] const char* context_type() const override { return "font"; }
    [[nodiscard]] std::vector<std::string> docked_window_names() const override;
    [[nodiscard]] std::string asset_path() const override { return recipe_path_; }
    [[nodiscard]] std::string document() const override;
    [[nodiscard]] Result<> write() override;
    [[nodiscard]] Result<> restore(const std::string& text) override;

    void init() override;
    void shutdown() override;
    void draw(float dt) override;
    void setup_dockspace(ImGuiID main_area) override;
    bool handle_shortcuts() override { return false; }

    [[nodiscard]] std::vector<ViewportRenderRequest> build_render_requests() override { return {}; }
    void apply_resize(VulkanContext& /*vk*/) override {}
    void render_previews(VulkanContext& vk, Scene* scene, float dt,
                         const EnvironmentState& environment, ThreadPool& pool) override;

    /// Opens a recipe by its project-relative path.
    void open_font(const std::string& recipe_path);

    /// Replaces the recipe being edited, writes it and asks for a rebuild.
    /// What the edit command calls in both directions.
    void apply_recipe(const FjFontRecipe& recipe);

private:
    void draw_recipe_panel();
    void draw_preview_panel();
    void commit(FjFontRecipe next, const char* what);

    FontEditorDeps deps_;
    std::string name_{"Font"};
    std::string recipe_path_;      // as opened, project-relative
    std::string recipe_abs_path_;
    FjFontRecipe recipe_;
    bool loaded_{false};

    // Field buffers, committed on Enter or when the field loses focus.
    char source_buf_[256]{};
    char ranges_buf_[256]{};
    int cell_size_edit_{48};
    float px_range_edit_{6.0f};

    // Sample line
    char sample_buf_[128]{};
    float sample_size_{64.0f};
    bool sample_dirty_{true};
    std::unique_ptr<GameUi> preview_ui_;
    std::unique_ptr<OffscreenPass> preview_target_;
    // The target matches the panel so the sample draws at its real size;
    // the panel reports what it has room for and the render follows.
    uint32_t preview_width_{0};
    uint32_t preview_height_{0};
    uint32_t wanted_width_{0};
    uint32_t wanted_height_{0};
};

} // namespace fjell
