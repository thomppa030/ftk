#pragma once

#include "ui/game_ui/ui_node.hpp"

#include <functional>

struct YGNode;
struct YGConfig;

namespace fjell {

/// How much room a run of text takes when drawn, in layout pixels.
struct UiTextSize {
    float width{0.0f};
    float height{0.0f};
};

/// Measures a text node as it would be drawn: shaped in its font, wrapped at
/// `max_width` when that is above 0.
using UiTextMeasure = std::function<UiTextSize(const UiNode& text, float max_width)>;

/// Builds a Yoga flexbox tree from UiNode styles, computes layout, reads back rects.
class UiLayout {
public:
    UiLayout();
    ~UiLayout();

    UiLayout(const UiLayout&) = delete;
    UiLayout& operator=(const UiLayout&) = delete;
    UiLayout(UiLayout&&) = delete;
    UiLayout& operator=(UiLayout&&) = delete;

    /// Build yoga nodes from the UI tree, compute layout at given viewport size,
    /// and write layout_rect back to each UiNode.
    /// pixel_scale is how many screen pixels one layout pixel covers; layout
    /// results are rounded so that edges land on whole screen pixels.
    void compute(UiNode& root, float viewport_width, float viewport_height,
                 float pixel_scale = 1.0f);

    /// Measures text, so a <text> element is as wide as what it shows and a
    /// row can place what follows it. Without one, text takes no room of its
    /// own and a sheet has to size it by hand.
    void set_text_measure(UiTextMeasure measure) { text_measure_ = std::move(measure); }

private:
    void build_yoga_tree(UiNode& node, YGNode* parent);
    /// Feeds a node's style to Yoga, turning font- and viewport-relative
    /// lengths into pixels on the way.
    void apply_style(YGNode* yg, const UiComputedStyle& style);
    void read_layout(UiNode& node, float offset_x, float offset_y);
    void propagate_clip(UiNode& node);

    YGConfig* config_{nullptr};
    YGNode* root_yg_{nullptr};
    UiTextMeasure text_measure_;
    // What em/rem/vw/vh measure against for the tree being computed.
    UiLengthContext length_context_{};
};

} // namespace fjell
