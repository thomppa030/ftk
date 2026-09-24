#pragma once

#include "ui/theme.hpp"

#include <optional>
#include <span>
#include <string>

// Several of one thing shown one at a time inside a panel, a VFX effect's
// emitters (sheet 25): a tab each along the panel's top, the selected one
// in front with an edge in its category colour, then a last tab that adds
// one. Hovering a tab shows its ×; a double-click asks to rename it.

namespace fjell::ui {

struct TabStripSpec {
    std::span<const std::string> names;
    int selected{0};
    theme::Category category{theme::Category::Structure};
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
/// when chosen. Draw the ImGui tab bar inside it.
///
///     if (auto bar = ui::ContextTabBar("##ContextBar")) {
///         if (ImGui::BeginTabBar("##ContextTabs", ...)) { ... }
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

} // namespace fjell::ui
