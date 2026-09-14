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

    // Dimensions. auto on width/height means content-sized; on the min/max
    // bounds it means the bound is not applied — the layout engine's own
    // default rather than an authorable keyword.
    UiLength width{UiLength::automatic()};
    UiLength height{UiLength::automatic()};
    UiLength min_width{UiLength::automatic()};
    UiLength min_height{UiLength::automatic()};
    UiLength max_width{UiLength::automatic()};
    UiLength max_height{UiLength::automatic()};

    // Spacing
    UiEdges margin{};
    UiEdges padding{};
    // Insets start out auto — "not positioned along this edge" — so an
    // authored `top: 0` is a real offset, not the same as never setting it.
    UiEdges inset{UiEdges::all(UiLength::automatic())};

    // Gap
    UiLength row_gap{};
    UiLength column_gap{};

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

    // Transition spec (parsed by UiAnimation)
    std::string transition;
};

} // namespace fjell
