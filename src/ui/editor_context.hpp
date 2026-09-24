#pragma once

#include "core/command_history.hpp"
#include "core/result.hpp"
#include "ui/document_history.hpp"
#include "ui/history_panel.hpp"
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

    /// Draws the context and records an undo step for any edit that
    /// finished this frame: the document before and after it. The engine
    /// calls this rather than draw().
    void draw_frame(float dt);

    // Build default dockspace layout for this context (called within DockBuilder block)
    virtual void setup_dockspace(ImGuiID main_area) = 0;

    // Handle context-specific shortcuts. Return true if consumed.
    virtual bool handle_shortcuts() = 0;

    [[nodiscard]] virtual std::string asset_path() const { return {}; }

    // Saving. A context's document is what it saves, written as text: the
    // JSON its file holds, or for a binary file the parts the context edits.
    // Unsaved changes are the document differing from the text last opened
    // or saved, so undoing back to the saved state leaves nothing unsaved and
    // no edit can forget to say it changed something.

    /// The edited asset as text; empty while nothing is open.
    [[nodiscard]] virtual std::string document() const { return {}; }

    /// Writes the document to its file or files. What went wrong on failure.
    [[nodiscard]] virtual Result<> write() { return {}; }

    /// Puts the document back as text document() wrote. Undo and redo of
    /// an edit made directly, and Revert, go through it.
    [[nodiscard]] virtual Result<> restore(const std::string& /*text*/) {
        return make_error("this editor can't restore its document");
    }

    /// Writes the document and remembers it as saved.
    Result<> save();

    /// True while the document differs from the one last opened or saved.
    /// The tab bar marks the context's tab with it and the window title
    /// follows the active context.
    [[nodiscard]] virtual bool has_unsaved_changes() const;

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
                                  const struct EnvironmentState& /*environment*/,
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
    void init_shared_panels(VulkanContext* vk) {
        stats_panel_.init(vk);
    }
    void draw_shared_panels(float dt);

    [[nodiscard]] StatsPanel& stats_panel() { return stats_panel_; }
    [[nodiscard]] HistoryPanel& history_panel() { return history_panel_; }

protected:
    /// Called once a document has been opened or reloaded from disk: what the
    /// context holds now is what the file holds.
    void document_opened();

    /// The document as last opened or saved, for a context that reloads one
    /// part of it (a different clip) and keeps the rest as it was.
    [[nodiscard]] const std::string& saved_document() const { return saved_document_; }
    void set_saved_document(std::string text);

private:
    // The document as last opened or saved, and the answer
    // has_unsaved_changes() gave, which it recomputes a few times a second
    // at most: a document can be large and the tab bar asks every frame.
    std::string saved_document_;
    mutable bool unsaved_{false};
    mutable double unsaved_checked_at_{-1.0};

    CommandHistory command_history_;
    // Undo for edits made directly to the document (declared after the
    // history it records into).
    DocumentHistory document_history_{
        command_history_, [this] { return document(); },
        [this](const std::string& text) { return restore_document(text); }};

    /// restore(), and the unsaved check redone after it.
    [[nodiscard]] Result<> restore_document(const std::string& text);
    StatsPanel stats_panel_;
    HistoryPanel history_panel_;
    ImGuiID dockspace_id_{0};
    int context_index_{0};
    bool dockspace_built_{false};
};

} // namespace fjell
