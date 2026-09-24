#include "ui/game_ui/ui_layout.hpp"
#include "core/profiler.hpp"

#include <yoga/Yoga.h>

#include <cmath>

namespace fjell {

namespace {

/// Yoga has a separate entry point per unit, so every length goes through
/// the same three-way dispatch. Properties Yoga has no auto variant for
/// pass nullptr and stay at Yoga's default when the length is auto.
struct LengthSetters {
    void (*px)(YGNodeRef, float);
    void (*percent)(YGNodeRef, float);
    void (*automatic)(YGNodeRef);
};

void apply_length(YGNodeRef yg, UiLength length, const UiLengthContext& ctx, LengthSetters set) {
    length = resolve_relative(length, ctx);
    switch (length.unit) {
    case UiUnit::px: set.px(yg, length.value); break;
    case UiUnit::percent: set.percent(yg, length.value); break;
    case UiUnit::auto_: if (set.automatic) set.automatic(yg); break;
    default: break; // resolved above
    }
}

struct EdgeSetters {
    void (*px)(YGNodeRef, YGEdge, float);
    void (*percent)(YGNodeRef, YGEdge, float);
    void (*automatic)(YGNodeRef, YGEdge);
};

void apply_edge(YGNodeRef yg, YGEdge edge, UiLength length, const UiLengthContext& ctx,
                EdgeSetters set) {
    length = resolve_relative(length, ctx);
    switch (length.unit) {
    case UiUnit::px: set.px(yg, edge, length.value); break;
    case UiUnit::percent: set.percent(yg, edge, length.value); break;
    case UiUnit::auto_: if (set.automatic) set.automatic(yg, edge); break;
    default: break;
    }
}

void apply_edges(YGNodeRef yg, const UiEdges& edges, const UiLengthContext& ctx, EdgeSetters set) {
    apply_edge(yg, YGEdgeTop, edges.top, ctx, set);
    apply_edge(yg, YGEdgeRight, edges.right, ctx, set);
    apply_edge(yg, YGEdgeBottom, edges.bottom, ctx, set);
    apply_edge(yg, YGEdgeLeft, edges.left, ctx, set);
}

void apply_gap(YGNodeRef yg, YGGutter gutter, UiLength length, const UiLengthContext& ctx) {
    length = resolve_relative(length, ctx);
    switch (length.unit) {
    case UiUnit::px: YGNodeStyleSetGap(yg, gutter, length.value); break;
    case UiUnit::percent: YGNodeStyleSetGapPercent(yg, gutter, length.value); break;
    default: break;
    }
}

} // namespace

namespace {

// Yoga's measure callback for a text node: the node rides along as the Yoga
// node's context and the layout that measures it as the config's.
YGSize measure_text(YGNodeConstRef yg, float width, YGMeasureMode width_mode,
                    float /*height*/, YGMeasureMode /*height_mode*/) {
    const auto* node = static_cast<const UiNode*>(YGNodeGetContext(yg));
    // YGNodeGetConfig takes a mutable node although it only reads it.
    YGConfigConstRef config = YGNodeGetConfig(const_cast<YGNodeRef>(yg));
    const auto* measure = static_cast<const UiTextMeasure*>(YGConfigGetContext(config));
    float max_width = width_mode == YGMeasureModeUndefined ? 0.0f : width;
    UiTextSize size = (*measure)(*node, max_width);

    // Whole pixels of the grid layout rounds to, and never fewer than the
    // text needs. Yoga rounds this leaf up, but the <text> element around it
    // rounds its edges to the nearest pixel, and the text is drawn wrapped at
    // that element's width: a hair short of it and the word drops a line.
    float scale = YGConfigGetPointScaleFactor(config);
    auto whole = [scale](float v) { return scale > 0.0f ? std::ceil(v * scale) / scale : v; };
    return {whole(size.width), whole(size.height)};
}

} // namespace

UiLayout::UiLayout() {
    config_ = YGConfigNew();
    YGConfigSetPointScaleFactor(config_, 1.0f);
    YGConfigSetContext(config_, &text_measure_);
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

void UiLayout::compute(UiNode& root, float viewport_width, float viewport_height,
                       float pixel_scale) {
    FJELL_PROFILE_SCOPE_N("ui_layout_compute");
    YGConfigSetPointScaleFactor(config_, pixel_scale);
    // Tear down previous tree
    if (root_yg_) {
        YGNodeFreeRecursive(root_yg_);
        root_yg_ = nullptr;
    }

    root_yg_ = YGNodeNewWithConfig(config_);
    root.yg_node = root_yg_;
    length_context_ = {root.style.font_size, root.style.font_size, viewport_width, viewport_height};

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
    // An element an if= hides takes no space, and neither does a foreach
    // template, which binding hides the same way; otherwise every list
    // would start one invisible item in.
    if (node.hidden_by_if) YGNodeStyleSetDisplay(yg, YGDisplayNone);

    // Text is a leaf Yoga asks for its size, so the <text> element around it
    // grows to fit what it shows.
    if (node.type == UiNodeType::text && text_measure_) {
        YGNodeSetContext(yg, &node);
        YGNodeSetMeasureFunc(yg, &measure_text);
    }

    auto index = YGNodeGetChildCount(parent);
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

    // Dimensions. The min/max bounds have no auto in Yoga; auto there means
    // the bound is simply not applied.
    // em measures against this node's own font size; the rest of the
    // context is shared across the tree.
    UiLengthContext ctx = length_context_;
    ctx.font_size = style.font_size;

    apply_length(yg, style.width, ctx,
                 {YGNodeStyleSetWidth, YGNodeStyleSetWidthPercent, YGNodeStyleSetWidthAuto});
    apply_length(yg, style.height, ctx,
                 {YGNodeStyleSetHeight, YGNodeStyleSetHeightPercent, YGNodeStyleSetHeightAuto});
    apply_length(yg, style.min_width, ctx,
                 {YGNodeStyleSetMinWidth, YGNodeStyleSetMinWidthPercent, nullptr});
    apply_length(yg, style.min_height, ctx,
                 {YGNodeStyleSetMinHeight, YGNodeStyleSetMinHeightPercent, nullptr});
    apply_length(yg, style.max_width, ctx,
                 {YGNodeStyleSetMaxWidth, YGNodeStyleSetMaxWidthPercent, nullptr});
    apply_length(yg, style.max_height, ctx,
                 {YGNodeStyleSetMaxHeight, YGNodeStyleSetMaxHeightPercent, nullptr});

    apply_edges(yg, style.margin, ctx,
                {YGNodeStyleSetMargin, YGNodeStyleSetMarginPercent, YGNodeStyleSetMarginAuto});
    apply_edges(yg, style.padding, ctx,
                {YGNodeStyleSetPadding, YGNodeStyleSetPaddingPercent, nullptr});

    // Border (Yoga uses border for layout spacing, we also draw it visually)
    if (style.border_width > 0) {
        YGNodeStyleSetBorder(yg, YGEdgeAll, style.border_width);
    }

    // Position insets: an auto edge is left unpositioned, so `top: 0` pins
    // to the top like any other value.
    apply_edges(yg, style.inset, ctx,
                {YGNodeStyleSetPosition, YGNodeStyleSetPositionPercent, YGNodeStyleSetPositionAuto});

    apply_gap(yg, YGGutterRow, style.row_gap, ctx);
    apply_gap(yg, YGGutterColumn, style.column_gap, ctx);
}

void UiLayout::read_layout(UiNode& node, float offset_x, float offset_y) {
    if (!node.yg_node) return;

    float x = YGNodeLayoutGetLeft(node.yg_node) + offset_x;
    float y = YGNodeLayoutGetTop(node.yg_node) + offset_y;
    float w = YGNodeLayoutGetWidth(node.yg_node);
    float h = YGNodeLayoutGetHeight(node.yg_node);

    node.layout_rect = {x, y, w, h};
    node.layout_margin = {YGNodeLayoutGetMargin(node.yg_node, YGEdgeTop),
                          YGNodeLayoutGetMargin(node.yg_node, YGEdgeRight),
                          YGNodeLayoutGetMargin(node.yg_node, YGEdgeBottom),
                          YGNodeLayoutGetMargin(node.yg_node, YGEdgeLeft)};
    node.layout_padding = {YGNodeLayoutGetPadding(node.yg_node, YGEdgeTop),
                           YGNodeLayoutGetPadding(node.yg_node, YGEdgeRight),
                           YGNodeLayoutGetPadding(node.yg_node, YGEdgeBottom),
                           YGNodeLayoutGetPadding(node.yg_node, YGEdgeLeft)};

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
