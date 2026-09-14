#pragma once

#include "ui/game_ui/fjss_parser.hpp"
#include "ui/game_ui/ui_node.hpp"
#include "ui/game_ui/ui_style_properties.hpp"

#include <string>
#include <unordered_map>

namespace fjell {

/// Where one computed property value came from: the winning declaration
/// and the rule that holds it, or nothing when no matching rule sets the
/// property and it is inherited or at its default.
struct StyleOrigin {
    const StyleRule* rule{nullptr};
    const StyleDeclaration* declaration{nullptr};
    bool inline_style{false}; ///< a `style-*` attribute on the node itself
};

class StyleResolver {
public:
    /// Apply stylesheet rules to a UI node tree, computing styles per node.
    void resolve(UiNode& root, const Stylesheet& sheet);

    /// The winning declaration for each property the cascade set on `node`,
    /// keyed by property name, in the same order apply_cascade applies them.
    /// A property missing from the result is inherited or at its default.
    [[nodiscard]] static std::unordered_map<std::string, StyleOrigin>
    explain(const UiNode& node, const Stylesheet& sheet);

    /// Re-resolve a node and everything below it, e.g. when its interaction
    /// state changes. Descendants are included because inherited values
    /// (color, font-size, opacity) flow down from the node's new style:
    /// a ".card:hover { color: red }" must recolor the text inside the card.
    void resolve_subtree(UiNode& node, const Stylesheet& sheet);

    /// Store the active stylesheet for targeted re-resolves.
    void set_stylesheet(const Stylesheet& sheet) { active_sheet_ = &sheet; }

    /// Viewport size the vw/vh units of `font-size` resolve against. Layout
    /// resolves them for every other property; font size has to be pixels
    /// by the end of the cascade because children inherit it and em builds
    /// on it.
    void set_viewport(float width, float height) {
        viewport_width_ = width;
        viewport_height_ = height;
    }

    /// Parse a single CSS-style value in isolation — exposed as a
    /// reusable pure function for callers outside the cascade (e.g. the
    /// UI editor's style inspector reading/writing individual property
    /// values without running a full resolve).
    static UiColor parse_color(const std::string& value);
    /// A bare number, with any trailing unit text ignored.
    static float parse_px(const std::string& value);
    /// `auto`, a percentage (`50%`) or pixels (`16`, `16px`).
    static UiLength parse_length(const std::string& value);

private:
    void resolve_node(UiNode& node, const Stylesheet& sheet,
                      const UiComputedStyle* parent_style);
    /// Computes one node's style from scratch: defaults, inherited values
    /// from parent_style, then every matching rule in cascade order.
    void apply_cascade(UiNode& node, const Stylesheet& sheet,
                       const UiComputedStyle* parent_style);
    void inherit_properties(UiComputedStyle& style, const UiComputedStyle& parent);
    [[nodiscard]] static bool selector_matches(const UiNode& node, const StyleSelector& sel);
    [[nodiscard]] static bool compound_matches(const UiNode& node, const StyleCompound& compound);
    /// Matches `steps[0..remaining)` against the chain above `current`,
    /// innermost step first.
    [[nodiscard]] static bool ancestors_match(const UiNode* current,
                                              const std::vector<StyleAncestorStep>& steps,
                                              size_t remaining);
    void apply_property(UiComputedStyle& style, const std::string& name, const std::string& value);
    /// The few properties whose syntax is their own: `gap`, `background`,
    /// `border`, `box-shadow`, `transition`.
    void apply_composite(UiComputedStyle& style, const UiPropertyDescriptor& desc,
                         const std::string& value);
    static UiEdges parse_edges(const std::string& value);
    static UiCorners parse_corners(const std::string& value);

    /// `font-size` needs what its units refer to: the parent's size for em
    /// and %, the root's for rem, the viewport for vw/vh.
    [[nodiscard]] float resolve_font_size(UiLength authored) const;

    const Stylesheet* active_sheet_{nullptr};
    float viewport_width_{0.0f};
    float viewport_height_{0.0f};
    // Set for the node being cascaded, see apply_cascade.
    float parent_font_size_{16.0f};
    float root_font_size_{16.0f};
};

} // namespace fjell
