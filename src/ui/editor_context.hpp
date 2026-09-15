#pragma once

#include "core/command_history.hpp"
#include "ui/history_panel.hpp"
#include "ui/project_settings_panel.hpp"
#include "ui/stats_panel.hpp"
#include "renderer/viewport_source.hpp"

#include <imgui.h>

#include <cstdio>
#include <string>
#include <vector>

namespace fjell {

class Scene;
class VulkanContext;

class EditorContext {
public:
    virtual ~EditorContext() = default;
    EditorContext() = default;
    EditorContext(const EditorContext&) = delete;
    EditorContext& operator=(const EditorContext&) = delete;
    EditorContext(EditorContext&&) = delete;
    EditorContext& operator=(EditorContext&&) = delete;

    [[nodiscard]] virtual const char* name() const = 0;
    [[nodiscard]] virtual const char* context_type() const = 0;
    [[nodiscard]] virtual std::vector<std::string> docked_window_names() const = 0;

    // Per-context undo/redo
    [[nodiscard]] CommandHistory& command_history() { return command_history_; }

    virtual void init() = 0;
    virtual void on_enter() {}
    virtual void on_exit() {}
    virtual void shutdown() {}

    // Draw context-owned panels (viewports + context-specific panels)
    virtual void draw(float dt) = 0;

    // Build default dockspace layout for this context (called within DockBuilder block)
    virtual void setup_dockspace(ImGuiID main_area) = 0;

    // Handle context-specific shortcuts. Return true if consumed.
    virtual bool handle_shortcuts() = 0;

    // Asset path and save support
    [[nodiscard]] virtual std::string asset_path() const { return {}; }
    virtual bool save() { return false; }

    // Fullscreen play presentation. Only the scene context hosts play.
    virtual void draw_play_overlay() {}

    // Scene to render (nullptr = use main scene)
    [[nodiscard]] virtual Scene* render_scene() { return nullptr; }

    // Viewport render requests
    [[nodiscard]] virtual std::vector<ViewportRenderRequest> build_render_requests() = 0;
    virtual void apply_resize(VulkanContext& vk) = 0;

    // Render auxiliary viewports (e.g. camera preview) in separate command buffers.
    // Called after apply_resize but before record_frame.
    virtual void render_previews(VulkanContext& /*vk*/, Scene* /*scene*/, float /*dt*/,
                                  const struct DirectionalLight& /*light*/,
                                  class ThreadPool& /*pool*/) {}

    // Dockspace layout tracking
    /// The requests of an asset preview: the same viewports, drawn over the
    /// editor's studio backdrop instead of the scene's sky, so the asset is
    /// what stands out.
    [[nodiscard]] static std::vector<ViewportRenderRequest> preview_render_requests(
        std::vector<ViewportRenderRequest> requests) {
        for (auto& request : requests) request.backdrop = &SkyBackdrop::editor_preview();
        return requests;
    }

    [[nodiscard]] bool dockspace_built() const { return dockspace_built_; }
    void mark_dockspace_built() { dockspace_built_ = true; }

    // Per-context dockspace ID and index (set by Engine)
    void set_dockspace_id(ImGuiID id) { dockspace_id_ = id; }
    [[nodiscard]] ImGuiID dockspace_id() const { return dockspace_id_; }
    void set_context_index(int idx) { context_index_ = idx; }
    [[nodiscard]] int context_index() const { return context_index_; }

    // Format a window title with context suffix: "Name" → "Name##ctx3"
    [[nodiscard]] std::string ctx_title(const char* base) const {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s##ctx%d", base, context_index_);
        return buf;
    }

    // Shared panels — each context owns its own instances so they dock correctly
    void init_shared_panels(VulkanContext* vk, class ProjectManager* projects) {
        stats_panel_.init(vk);
        project_settings_panel_.init(vk, projects);
    }
    void draw_shared_panels(float dt);

    [[nodiscard]] StatsPanel& stats_panel() { return stats_panel_; }
    [[nodiscard]] HistoryPanel& history_panel() { return history_panel_; }
    [[nodiscard]] ProjectSettingsPanel& project_settings_panel() { return project_settings_panel_; }

private:
    CommandHistory command_history_;
    StatsPanel stats_panel_;
    HistoryPanel history_panel_;
    ProjectSettingsPanel project_settings_panel_;
    ImGuiID dockspace_id_{0};
    int context_index_{0};
    bool dockspace_built_{false};
};

} // namespace fjell
