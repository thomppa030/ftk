#pragma once

#include "ui/game_ui/fjss_parser.hpp"
#include "ui/game_ui/ui_node.hpp"

#include <string>
#include <unordered_set>

namespace fjell {

class UiContext;

/// Post-parse tree walk that resolves unknown tags to widget .fjui/.fjss files.
///
/// Unknown tags (anything not in the built-in set) are looked up as
/// <base_dir>/widgets/<tag_name>.fjui (hyphens replaced with underscores).
/// The widget's subtree replaces the custom tag's children, its stylesheet
/// rules are merged into the main stylesheet with scope prefixes, and a
/// scoped UiContext is created for the widget instance.
class WidgetResolver {
public:
    /// Resolve all widget tags in the tree.
    /// base_dir: directory containing the root .fjui (widget files at base_dir/widgets/)
    /// root_context: the root UiContext (parent for widget scoped contexts)
    void resolve(UiNode& root, const std::string& base_dir,
                 Stylesheet& stylesheet, UiContext& root_context);

private:
    void resolve_node(UiNode& node, const std::string& base_dir,
                      Stylesheet& stylesheet, UiContext& parent_context,
                      std::unordered_set<std::string>& resolution_stack);

    static bool is_builtin_tag(const std::string& tag);
    static std::string tag_to_filename(const std::string& tag);

    uint32_t scope_counter_{0};
};

} // namespace fjell
