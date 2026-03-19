#pragma once

#include "ui/game_ui/ui_types.hpp"

namespace fjell {

struct UiComputedStyle {
    // Layout
    UiDisplay display{UiDisplay::flex};
    UiFlexDirection flex_direction{UiFlexDirection::column};
    UiJustify justify_content{UiJustify::flex_start};
    UiAlign align_items{UiAlign::stretch};
    UiAlign align_self{UiAlign::auto_};
    UiFlexWrap flex_wrap{UiFlexWrap::no_wrap};
    UiPosition position{UiPosition::relative};
    UiOverflow overflow{UiOverflow::visible};

    float flex_grow{0.0f};
    float flex_shrink{1.0f};

    // Dimensions (-1 = auto)
    float width{-1.0f};
    float height{-1.0f};
    float min_width{-1.0f};
    float min_height{-1.0f};
    float max_width{-1.0f};
    float max_height{-1.0f};

    // Spacing
    UiEdges margin{};
    UiEdges padding{};
    UiEdges inset{}; // top/right/bottom/left for position:absolute

    // Gap
    float row_gap{0.0f};
    float column_gap{0.0f};

    // Visual
    UiColor background_color{0.0f, 0.0f, 0.0f, 0.0f};
    UiColor border_color{0.0f, 0.0f, 0.0f, 0.0f};
    float border_width{0.0f};
    UiCorners border_radius{};
    float opacity{1.0f};

    // Text
    UiColor color{1.0f, 1.0f, 1.0f, 1.0f};
    float font_size{16.0f};
    float line_height{0.0f}; // 0 = use font metric default

    // Effects
    UiBoxShadow box_shadow{};
    UiGradient gradient{};
};

} // namespace fjell
