#pragma once

#include "ftk/app/command_history.hpp"
#include "ftk/app/document_history.hpp"
#include "ftk/app/history_panel.hpp"
#include "ftk/base/result.hpp"

#include <imgui.h>

#include <cstdio>
#include <string>
#include <vector>

namespace fjell {

/// One tab of an editor: the document it edits, its undo history, its docked
/// panels and how it draws. A host derives its own kind of context from this,
/// with whatever else its editors need, and each asset editor from that.
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
    /// Where an asset editor's panels start (sheet 7), by their titles:
    /// a tree or lists down the left, the preview, what sits under it (a
    /// graph, timeline or canvas), the inspector down the right. The
    /// bottom_tabs() always sit along the bottom, under the preview. Empty
    /// slots are left out.
    struct DockPreset {
        std::vector<std::string> tree{};
        std::vector<std::string> preview{};
        std::vector<std::string> under_preview{};
        std::vector<std::string> inspector{};
    };
    [[nodiscard]] virtual DockPreset dock_preset() const { return {}; }

    /// Every window the context docks, with its context suffix: the
    /// preset's panels and the bottom tabs.
    [[nodiscard]] virtual std::vector<std::string> docked_window_names() const;

    /// The panels along the bottom, in tab order: the console and the undo
    /// history. A host adds its own and draws them in draw_shared_panels().
    [[nodiscard]] virtual std::vector<std::string> bottom_tabs() const;

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

    // Build default dockspace layout for this context (called within
    // DockBuilder block): the preset's, unless the context lays out its own.
    virtual void setup_dockspace(ImGuiID main_area);

    /// The editor's own actions in the header (open the shader, play in the
    /// scene), each drawn after ImGui::SameLine(). None by default.
    virtual void draw_header_actions() {}

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

    /// Writes the document and remembers it as saved. A failure is shown
    /// under the header until a save works.
    Result<> save();

    /// Puts the document back as last opened or saved, as one undo step.
    void revert();

    /// The files Save writes, for its tooltip: the asset's own file unless
    /// the editor writes more ("hero.fjanimset, walk.fjanim and hero.fjskel").
    [[nodiscard]] virtual std::string saved_files() const;

    /// Draws the header strip across the context (sheet 7) when it edits an
    /// asset. The engine calls this under the context tabs, before the
    /// dockspace.
    void draw_header();

    /// True while the document differs from the one last opened or saved.
    /// The tab bar marks the context's tab with it and the window title
    /// follows the active context.
    [[nodiscard]] virtual bool has_unsaved_changes() const;

    // Dockspace layout tracking
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

    /// Draws the bottom tabs. Each context owns its own panels, so they dock
    /// with it; an override that adds tabs draws its own and calls this.
    virtual void draw_shared_panels(float dt);

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
    // Why the last save failed; empty once one works.
    std::string save_error_;
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
    HistoryPanel history_panel_;
    ImGuiID dockspace_id_{0};
    int context_index_{0};
    bool dockspace_built_{false};
};

} // namespace fjell
