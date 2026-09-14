#pragma once

#include "ui/game_ui/fjss_parser.hpp"
#include "ui/game_ui/ui_node.hpp"

namespace fjell {

class StyleResolver {
public:
    /// Apply stylesheet rules to a UI node tree, computing styles per node.
    void resolve(UiNode& root, const Stylesheet& sheet);

    /// Re-resolve a node and everything below it, e.g. when its interaction
    /// state changes. Descendants are included because inherited values
    /// (color, font-size, opacity) flow down from the node's new style:
    /// a ".card:hover { color: red }" must recolor the text inside the card.
    void resolve_subtree(UiNode& node, const Stylesheet& sheet);

    /// Store the active stylesheet for targeted re-resolves.
    void set_stylesheet(const Stylesheet& sheet) { active_sheet_ = &sheet; }

    /// Parse a single CSS-style value in isolation — exposed as a
    /// reusable pure function for callers outside the cascade (e.g. the
    /// UI editor's style inspector reading/writing individual property
    /// values without running a full resolve).
    static UiColor parse_color(const std::string& value);
    static float parse_px(const std::string& value);

private:
    void resolve_node(UiNode& node, const Stylesheet& sheet,
                      const UiComputedStyle* parent_style);
    /// Computes one node's style from scratch: defaults, inherited values
    /// from parent_style, then every matching rule in cascade order.
    void apply_cascade(UiNode& node, const Stylesheet& sheet,
                       const UiComputedStyle* parent_style);
    void inherit_properties(UiComputedStyle& style, const UiComputedStyle& parent);
    [[nodiscard]] bool selector_matches(const UiNode& node, const StyleSelector& sel) const;
    void apply_property(UiComputedStyle& style, const std::string& name, const std::string& value);
    static UiEdges parse_edges(const std::string& value);
    static UiCorners parse_corners(const std::string& value);

    const Stylesheet* active_sheet_{nullptr};
};

} // namespace fjell
