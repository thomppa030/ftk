#pragma once

#include "ui/game_ui/fjss_parser.hpp"
#include "ui/game_ui/ui_node.hpp"
#include "ui/game_ui/widget_asset.hpp"

#include <string>
#include <unordered_map>
#include <unordered_set>

namespace fjell {

class UiContext;

/// Post-parse tree walk that resolves unknown tags to widget .fjwidget files.
///
/// Unknown tags (anything not in the built-in set) are looked up as
/// <base_dir>/widgets/<tag_name>.fjwidget (hyphens replaced with underscores).
/// The widget's subtree replaces the custom tag's children, its stylesheet
/// rules are merged into the main stylesheet with scope prefixes, and a
/// scoped UiContext is created for the widget instance.
class WidgetResolver {
public:
    /// Resolve all widget tags in the tree.
    /// base_dir: directory containing the root .fjwidget (widget files at base_dir/widgets/)
    /// root_context: the root UiContext (parent for widget scoped contexts)
    void resolve(UiNode& root, const std::string& base_dir,
                 Stylesheet& stylesheet, UiContext& root_context);

    /// Every file read while resolving the last tree — each .fjwidget and
    /// each widget stylesheet — so the owner can watch them for changes.
    [[nodiscard]] const std::vector<std::string>& dependencies() const { return dependencies_; }

private:
    void resolve_node(UiNode& node, const std::string& base_dir,
                      Stylesheet& stylesheet, UiContext& parent_context,
                      std::unordered_set<std::string>& resolution_stack);

    static bool is_builtin_tag(const std::string& tag);
    static std::string tag_to_filename(const std::string& tag);

    uint32_t scope_counter_{0};
    // Dedupes repeated uses of one widget within a single resolve. It is
    // cleared per resolve on purpose: kept across loads it would keep
    // serving markup from before the file changed on disk.
    std::unordered_map<std::string, WidgetAsset> widget_cache_;
    std::vector<std::string> dependencies_;
};

} // namespace fjell
