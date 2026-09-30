#pragma once

#include "ui/editor_context.hpp"
#include "ui/stats_panel.hpp"
#include "renderer/viewport_source.hpp"

#include <string>
#include <vector>

namespace fjell {

class Scene;
class ViewportManager;
class VulkanContext;

/// An editor context the engine drives. Beyond the shell's document, undo and
/// docking it says how its viewports render through the engine's renderer,
/// shows the play overlay if it hosts play, and adds Stats to the tabs along
/// the bottom. Every one of Fjell's editors derives from it.
class EngineEditorContext : public EditorContext {
public:
    /// Console, Stats and History, in that order.
    [[nodiscard]] std::vector<std::string> bottom_tabs() const override;
    void draw_shared_panels(float dt) override;

    // Fullscreen play presentation. Only the scene context hosts play.
    virtual void draw_play_overlay() {}

    // Scene to render (nullptr = use main scene)
    [[nodiscard]] virtual Scene* render_scene() { return nullptr; }

    // Viewport render requests
    [[nodiscard]] virtual std::vector<ViewportRenderRequest> build_render_requests() = 0;
    virtual void apply_resize(VulkanContext& vk) = 0;

    // Render auxiliary viewports (e.g. camera preview) into the frame, ahead
    // of its views. Called after apply_resize but before record_frame.
    virtual void render_previews(VulkanContext& /*vk*/, Scene* /*scene*/, float /*dt*/,
                                  const struct EnvironmentState& /*environment*/,
                                  class ThreadPool& /*pool*/) {}

    /// The requests of an asset preview: the same viewports, drawn over the
    /// editor's studio backdrop instead of the scene's sky, so the asset is
    /// what stands out.
    [[nodiscard]] static std::vector<ViewportRenderRequest> preview_render_requests(
        std::vector<ViewportRenderRequest> requests) {
        for (auto& request : requests) request.backdrop = &SkyBackdrop::editor_preview();
        return requests;
    }

    /// Names the context's preview window "Preview" with its context suffix,
    /// so it docks where the preset puts it. Called before drawing it.
    void name_preview(ViewportManager& viewports) const;

    // The Stats panel reads the renderer's numbers, so it is given the
    // context's renderer when the context starts.
    void init_shared_panels(VulkanContext* vk) {
        stats_panel_.init(vk);
    }

    [[nodiscard]] StatsPanel& stats_panel() { return stats_panel_; }

private:
    StatsPanel stats_panel_;
};

} // namespace fjell
