#pragma once

#include "ftk/ui/kit/dock.hpp"
#include "ftk/ui/theme.hpp"

#include <glm/vec2.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string>

// Several of one thing shown one at a time inside a panel, a VFX effect's
// emitters (sheet 25): a tab each along the panel's top, the selected one
// in front with an edge in its hue, then a last tab that adds
// one. Hovering a tab shows its ×; a double-click asks to rename it.

namespace ftk::ui {

struct TabStripSpec {
    std::span<const std::string> names;
    int selected{0};
    theme::Hue hue{theme::Hue::grey};
    /// The last tab's words, "Add emitter".
    const char* add_label{"Add"};
    /// What × removes, for its tooltip: "Remove emitter".
    const char* remove_label{"Remove"};
};

struct TabStripResult {
    /// The tab now in front, when it changed.
    std::optional<int> selected;
    /// A tab whose × was clicked. Never the last one left.
    std::optional<int> removed;
    /// A tab double-clicked, to rename.
    std::optional<int> rename;
    /// The Add tab was clicked.
    bool add{false};
};

TabStripResult tab_strip(const char* id, const TabStripSpec& spec);

/// The bar along the top of the editor holding a tab for each context (the
/// Scene and every open asset): a strip in the menu bar's colour, its tabs
/// at the kit's tab padding, raised under the mouse and the base surface
/// when chosen. Its tabs are DocumentTabs inside it.
///
///     if (auto bar = ui::ContextTabBar("##ContextBar")) {
///         if (auto tabs = ui::DocumentTabs("##ContextTabs")) {
///             for (const Doc& doc : docs) { ... ui::document_tab({.label = doc.name}) ... }
///         }
///     }
class ContextTabBar {
public:
    explicit ContextTabBar(const char* id);
    ~ContextTabBar();
    ContextTabBar(const ContextTabBar&) = delete;
    ContextTabBar& operator=(const ContextTabBar&) = delete;
    ContextTabBar(ContextTabBar&&) = delete;
    ContextTabBar& operator=(ContextTabBar&&) = delete;

    explicit operator bool() const { return open_; }

private:
    bool open_{false};
};

struct DocumentTabsOptions {
    /// Rows of one group, in any of the program's windows, take each other's
    /// tabs: one that can be torn off, dragged off its row onto another's,
    /// parts that row where it would land, and lands there when let go.
    /// Where the platform keeps windows' places to itself (Wayland), only
    /// rows of one window. Null for a row whose tabs stay in it.
    const char* group{nullptr};
    /// What the program knows the row by, told to a tab that lands on it.
    int owner{0};
};

/// A row of tabs, one for each document open, which can be put in another
/// order by dragging them. Its tabs are document_tab()s, drawn while it is
/// true.
class DocumentTabs {
public:
    explicit DocumentTabs(const char* id, const DocumentTabsOptions& options = {});
    ~DocumentTabs();
    DocumentTabs(const DocumentTabs&) = delete;
    DocumentTabs& operator=(const DocumentTabs&) = delete;
    DocumentTabs(DocumentTabs&&) = delete;
    DocumentTabs& operator=(DocumentTabs&&) = delete;

    explicit operator bool() const { return open_; }

private:
    bool open_{false};
};

struct DocumentTab {
    /// Its name, which is also its id; what follows "##" is part of the id
    /// and isn't shown, for two documents of one name.
    const char* label{""};
    /// Its kind's icon from ui::icon before the name, tinted in `hue`; in
    /// the secondary text colour without one.
    const char* icon{nullptr};
    std::optional<theme::Hue> hue{};
    /// It has a × to close it; not one that is always open.
    bool closable{true};
    /// It has edits not saved: a dot shows in place of the × until the
    /// mouse is on it.
    bool unsaved{false};
    /// It comes to the front this frame, as when it is opened elsewhere.
    bool select{false};
    /// It can be dragged off the row and let go past the window's edge, for
    /// the program to show it in a window of its own.
    bool tear_off{false};
    /// It can be dragged off the row and let go on a part of a dockspace of
    /// its window, the dock's targets showing as for a panel carried there,
    /// for the program to show it as a panel (DocumentTabResult::docked).
    /// Lifted off, it is carried until let go: held back over its row on
    /// another tab, that tab comes forward, and let go there it stays where
    /// it was. Beside a part, it takes `dock_share` of it.
    bool docks{false};
    float dock_share{0.5f};
};

struct DocumentTabResult {
    /// It is the tab in front.
    bool front{false};
    /// Its × was clicked.
    bool closed{false};
    /// A tab that can be torn off, dragged and let go past the window's edge
    /// this frame: where, in the window's layout units, for the window the
    /// program shows it in.
    std::optional<glm::vec2> torn_off{};
    /// Dragged onto another row of its group and let go there this frame:
    /// that row's owner, for the program to draw the tab there from now on.
    /// The row shows it where it was let go.
    std::optional<int> landed_on{};
    /// Its place in the row as shown, from 0, in the order dragging put the
    /// tabs in.
    std::size_t place{0};
    /// A tab that docks, carried off its row and held this frame: for the
    /// program to show under it what it would dock into, rather than itself.
    bool lifted{false};
    /// A tab that docks let go on a dock target this frame: where, for the
    /// program to dock what the tab shows there (dock_window_at()).
    std::optional<DockPlace> docked{};
};

/// One tab of the DocumentTabs being drawn.
DocumentTabResult document_tab(const DocumentTab& tab);

} // namespace ftk::ui
