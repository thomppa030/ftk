#pragma once

#include "ui/game_ui/ui_node.hpp"

struct YGNode;
struct YGConfig;

namespace fjell {

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
    void compute(UiNode& root, float viewport_width, float viewport_height);

private:
    void build_yoga_tree(UiNode& node, YGNode* parent);
    void apply_style(YGNode* yg, const UiComputedStyle& style);
    void read_layout(UiNode& node, float offset_x, float offset_y);
    void propagate_clip(UiNode& node);

    YGConfig* config_{nullptr};
    YGNode* root_yg_{nullptr};
};

} // namespace fjell
