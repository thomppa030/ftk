#pragma once

#include "ui/game_ui/fjss_parser.hpp"
#include "ui/game_ui/ui_node.hpp"

namespace fjell {

class StyleResolver {
public:
    /// Apply stylesheet rules to a UI node tree, computing styles per node.
    void resolve(UiNode& root, const Stylesheet& sheet);

private:
    void resolve_node(UiNode& node, const Stylesheet& sheet,
                      const UiComputedStyle* parent_style);
    void inherit_properties(UiComputedStyle& style, const UiComputedStyle& parent);
    [[nodiscard]] bool selector_matches(const UiNode& node, const StyleSelector& sel) const;
    void apply_property(UiComputedStyle& style, const std::string& name, const std::string& value);

    static UiColor parse_color(const std::string& value);
    static float parse_px(const std::string& value);
    static UiEdges parse_edges(const std::string& value);
    static UiCorners parse_corners(const std::string& value);
};

} // namespace fjell
