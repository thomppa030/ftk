#include "ui/game_ui/ui_layout.hpp"

#include <yoga/Yoga.h>

namespace fjell {

UiLayout::UiLayout() {
    config_ = YGConfigNew();
    YGConfigSetPointScaleFactor(config_, 1.0f);
}

UiLayout::~UiLayout() {
    if (root_yg_) {
        YGNodeFreeRecursive(root_yg_);
        root_yg_ = nullptr;
    }
    if (config_) {
        YGConfigFree(config_);
        config_ = nullptr;
    }
}

void UiLayout::compute(UiNode& root, float viewport_width, float viewport_height) {
    // Tear down previous tree
    if (root_yg_) {
        YGNodeFreeRecursive(root_yg_);
        root_yg_ = nullptr;
    }

    root_yg_ = YGNodeNewWithConfig(config_);
    root.yg_node = root_yg_;

    // Root node fills viewport
    YGNodeStyleSetWidth(root_yg_, viewport_width);
    YGNodeStyleSetHeight(root_yg_, viewport_height);

    apply_style(root_yg_, root.style);

    // Build yoga tree from children
    for (size_t i = 0; i < root.children.size(); ++i) {
        build_yoga_tree(*root.children[i], root_yg_);
    }

    YGNodeCalculateLayout(root_yg_, viewport_width, viewport_height, YGDirectionLTR);

    // Read back computed layout
    read_layout(root, 0.0f, 0.0f);

    // Propagate clip rects for overflow:hidden
    root.clip_rect = {0.0f, 0.0f, viewport_width, viewport_height};
    propagate_clip(root);
}

void UiLayout::build_yoga_tree(UiNode& node, YGNode* parent) {
    auto* yg = YGNodeNewWithConfig(config_);
    node.yg_node = yg;

    apply_style(yg, node.style);

    uint32_t index = YGNodeGetChildCount(parent);
    YGNodeInsertChild(parent, yg, index);

    for (auto& child : node.children) {
        build_yoga_tree(*child, yg);
    }
}

void UiLayout::apply_style(YGNode* yg, const UiComputedStyle& style) {
    // Display
    if (style.display == UiDisplay::none) {
        YGNodeStyleSetDisplay(yg, YGDisplayNone);
    } else {
        YGNodeStyleSetDisplay(yg, YGDisplayFlex);
    }

    // Flex direction
    switch (style.flex_direction) {
    case UiFlexDirection::row: YGNodeStyleSetFlexDirection(yg, YGFlexDirectionRow); break;
    case UiFlexDirection::column: YGNodeStyleSetFlexDirection(yg, YGFlexDirectionColumn); break;
    case UiFlexDirection::row_reverse: YGNodeStyleSetFlexDirection(yg, YGFlexDirectionRowReverse); break;
    case UiFlexDirection::column_reverse: YGNodeStyleSetFlexDirection(yg, YGFlexDirectionColumnReverse); break;
    }

    // Justify content
    switch (style.justify_content) {
    case UiJustify::flex_start: YGNodeStyleSetJustifyContent(yg, YGJustifyFlexStart); break;
    case UiJustify::flex_end: YGNodeStyleSetJustifyContent(yg, YGJustifyFlexEnd); break;
    case UiJustify::center: YGNodeStyleSetJustifyContent(yg, YGJustifyCenter); break;
    case UiJustify::space_between: YGNodeStyleSetJustifyContent(yg, YGJustifySpaceBetween); break;
    case UiJustify::space_around: YGNodeStyleSetJustifyContent(yg, YGJustifySpaceAround); break;
    case UiJustify::space_evenly: YGNodeStyleSetJustifyContent(yg, YGJustifySpaceEvenly); break;
    }

    // Align items
    switch (style.align_items) {
    case UiAlign::auto_: break;
    case UiAlign::flex_start: YGNodeStyleSetAlignItems(yg, YGAlignFlexStart); break;
    case UiAlign::flex_end: YGNodeStyleSetAlignItems(yg, YGAlignFlexEnd); break;
    case UiAlign::center: YGNodeStyleSetAlignItems(yg, YGAlignCenter); break;
    case UiAlign::stretch: YGNodeStyleSetAlignItems(yg, YGAlignStretch); break;
    case UiAlign::baseline: YGNodeStyleSetAlignItems(yg, YGAlignBaseline); break;
    }

    // Align self
    switch (style.align_self) {
    case UiAlign::auto_: YGNodeStyleSetAlignSelf(yg, YGAlignAuto); break;
    case UiAlign::flex_start: YGNodeStyleSetAlignSelf(yg, YGAlignFlexStart); break;
    case UiAlign::flex_end: YGNodeStyleSetAlignSelf(yg, YGAlignFlexEnd); break;
    case UiAlign::center: YGNodeStyleSetAlignSelf(yg, YGAlignCenter); break;
    case UiAlign::stretch: YGNodeStyleSetAlignSelf(yg, YGAlignStretch); break;
    case UiAlign::baseline: YGNodeStyleSetAlignSelf(yg, YGAlignBaseline); break;
    }

    // Flex wrap
    switch (style.flex_wrap) {
    case UiFlexWrap::no_wrap: YGNodeStyleSetFlexWrap(yg, YGWrapNoWrap); break;
    case UiFlexWrap::wrap: YGNodeStyleSetFlexWrap(yg, YGWrapWrap); break;
    case UiFlexWrap::wrap_reverse: YGNodeStyleSetFlexWrap(yg, YGWrapWrapReverse); break;
    }

    // Position type
    if (style.position == UiPosition::absolute) {
        YGNodeStyleSetPositionType(yg, YGPositionTypeAbsolute);
    } else {
        YGNodeStyleSetPositionType(yg, YGPositionTypeRelative);
    }

    // Overflow
    switch (style.overflow) {
    case UiOverflow::visible: YGNodeStyleSetOverflow(yg, YGOverflowVisible); break;
    case UiOverflow::hidden: YGNodeStyleSetOverflow(yg, YGOverflowHidden); break;
    case UiOverflow::scroll: YGNodeStyleSetOverflow(yg, YGOverflowScroll); break;
    }

    // Flex grow/shrink
    YGNodeStyleSetFlexGrow(yg, style.flex_grow);
    YGNodeStyleSetFlexShrink(yg, style.flex_shrink);

    // Dimensions
    if (style.width >= 0) YGNodeStyleSetWidth(yg, style.width);
    if (style.height >= 0) YGNodeStyleSetHeight(yg, style.height);
    if (style.min_width >= 0) YGNodeStyleSetMinWidth(yg, style.min_width);
    if (style.min_height >= 0) YGNodeStyleSetMinHeight(yg, style.min_height);
    if (style.max_width >= 0) YGNodeStyleSetMaxWidth(yg, style.max_width);
    if (style.max_height >= 0) YGNodeStyleSetMaxHeight(yg, style.max_height);

    // Margin
    YGNodeStyleSetMargin(yg, YGEdgeTop, style.margin.top);
    YGNodeStyleSetMargin(yg, YGEdgeRight, style.margin.right);
    YGNodeStyleSetMargin(yg, YGEdgeBottom, style.margin.bottom);
    YGNodeStyleSetMargin(yg, YGEdgeLeft, style.margin.left);

    // Padding
    YGNodeStyleSetPadding(yg, YGEdgeTop, style.padding.top);
    YGNodeStyleSetPadding(yg, YGEdgeRight, style.padding.right);
    YGNodeStyleSetPadding(yg, YGEdgeBottom, style.padding.bottom);
    YGNodeStyleSetPadding(yg, YGEdgeLeft, style.padding.left);

    // Border (Yoga uses border for layout spacing, we also draw it visually)
    if (style.border_width > 0) {
        YGNodeStyleSetBorder(yg, YGEdgeAll, style.border_width);
    }

    // Position insets (for absolute positioning)
    if (style.inset.top != 0) YGNodeStyleSetPosition(yg, YGEdgeTop, style.inset.top);
    if (style.inset.right != 0) YGNodeStyleSetPosition(yg, YGEdgeRight, style.inset.right);
    if (style.inset.bottom != 0) YGNodeStyleSetPosition(yg, YGEdgeBottom, style.inset.bottom);
    if (style.inset.left != 0) YGNodeStyleSetPosition(yg, YGEdgeLeft, style.inset.left);

    // Gap
    if (style.row_gap > 0) YGNodeStyleSetGap(yg, YGGutterRow, style.row_gap);
    if (style.column_gap > 0) YGNodeStyleSetGap(yg, YGGutterColumn, style.column_gap);
}

void UiLayout::read_layout(UiNode& node, float offset_x, float offset_y) {
    if (!node.yg_node) return;

    float x = YGNodeLayoutGetLeft(node.yg_node) + offset_x;
    float y = YGNodeLayoutGetTop(node.yg_node) + offset_y;
    float w = YGNodeLayoutGetWidth(node.yg_node);
    float h = YGNodeLayoutGetHeight(node.yg_node);

    node.layout_rect = {x, y, w, h};

    // Apply scroll offset for overflow:scroll containers
    float child_offset_y = y;
    if (node.style.overflow == UiOverflow::scroll) {
        child_offset_y -= node.scroll_offset_y;
    }

    for (auto& child : node.children) {
        read_layout(*child, x, child_offset_y);
    }
}

void UiLayout::propagate_clip(UiNode& node) {
    for (auto& child : node.children) {
        if (node.style.overflow == UiOverflow::hidden) {
            // Intersect parent clip with this node's rect
            float cx = std::max(node.clip_rect.x, node.layout_rect.x);
            float cy = std::max(node.clip_rect.y, node.layout_rect.y);
            float cx2 = std::min(node.clip_rect.x + node.clip_rect.w,
                                 node.layout_rect.x + node.layout_rect.w);
            float cy2 = std::min(node.clip_rect.y + node.clip_rect.h,
                                 node.layout_rect.y + node.layout_rect.h);
            child->clip_rect = {cx, cy, std::max(0.0f, cx2 - cx), std::max(0.0f, cy2 - cy)};
        } else {
            child->clip_rect = node.clip_rect;
        }
        propagate_clip(*child);
    }
}

} // namespace fjell
