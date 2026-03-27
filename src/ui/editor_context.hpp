#pragma once

#include "core/command_history.hpp"
#include "renderer/viewport_source.hpp"

#include <imgui.h>

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

    // Play mode
    [[nodiscard]] virtual bool supports_play() const { return false; }
    [[nodiscard]] virtual bool is_playing() const { return false; }
    virtual void draw_play_overlay() {}

    // Scene to render (nullptr = use main scene)
    [[nodiscard]] virtual Scene* render_scene() { return nullptr; }

    // Viewport render requests
    [[nodiscard]] virtual std::vector<ViewportRenderRequest> build_render_requests() = 0;
    virtual void apply_resize(VulkanContext& vk) = 0;
    [[nodiscard]] virtual bool any_viewport_hovered() const { return false; }

    // Dockspace layout tracking
    [[nodiscard]] bool dockspace_built() const { return dockspace_built_; }
    void mark_dockspace_built() { dockspace_built_ = true; }

    // Per-context dockspace ID and index (set by Engine)
    void set_dockspace_id(ImGuiID id) { dockspace_id_ = id; }
    [[nodiscard]] ImGuiID dockspace_id() const { return dockspace_id_; }
    void set_context_index(int idx) { context_index_ = idx; }
    [[nodiscard]] int context_index() const { return context_index_; }

private:
    CommandHistory command_history_;
    ImGuiID dockspace_id_{0};
    int context_index_{0};
    bool dockspace_built_{false};
};

} // namespace fjell
