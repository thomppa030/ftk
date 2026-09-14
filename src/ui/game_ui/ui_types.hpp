#pragma once

#include "ui/game_ui/ui_length.hpp"

#include <cstdint>
#include <string>

namespace fjell {

enum class UiNodeType : uint8_t {
    element,
    text,
    root,
};

enum class UiDisplay : uint8_t {
    flex,
    none,
};

enum class UiFlexDirection : uint8_t {
    row,
    column,
    row_reverse,
    column_reverse,
};

enum class UiJustify : uint8_t {
    flex_start,
    flex_end,
    center,
    space_between,
    space_around,
    space_evenly,
};

enum class UiAlign : uint8_t {
    auto_,
    flex_start,
    flex_end,
    center,
    stretch,
    baseline,
};

enum class UiPosition : uint8_t {
    relative,
    absolute,
};

enum class UiOverflow : uint8_t {
    visible,
    hidden,
    scroll,
};

enum class UiFlexWrap : uint8_t {
    no_wrap,
    wrap,
    wrap_reverse,
};

struct UiColor {
    float r{0.0f};
    float g{0.0f};
    float b{0.0f};
    float a{1.0f};

    static UiColor from_hex(uint32_t hex) {
        return {
            static_cast<float>((hex >> 16) & 0xFF) / 255.0f,
            static_cast<float>((hex >> 8) & 0xFF) / 255.0f,
            static_cast<float>(hex & 0xFF) / 255.0f,
            1.0f,
        };
    }

    static UiColor from_hex_alpha(uint32_t hex) {
        return {
            static_cast<float>((hex >> 24) & 0xFF) / 255.0f,
            static_cast<float>((hex >> 16) & 0xFF) / 255.0f,
            static_cast<float>((hex >> 8) & 0xFF) / 255.0f,
            static_cast<float>(hex & 0xFF) / 255.0f,
        };
    }
};

/// The frame a UI is designed in, and how it is scaled to whatever viewport
/// shows it. The frame is scaled to fit: by height on a viewport at least as
/// wide as the frame's aspect, by width on a narrower one, so the UI never
/// overflows either way. A zero width scales by height alone; a zero height
/// means no scaling, one layout pixel per screen pixel.
struct UiDesignFrame {
    float width{0.0f};
    float height{0.0f};

    /// Screen pixels per layout pixel for a viewport of the given size.
    [[nodiscard]] constexpr float scale_for(float viewport_width, float viewport_height) const {
        if (height <= 0.0f || viewport_height <= 0.0f) return 1.0f;
        float scale = viewport_height / height;
        if (width > 0.0f && viewport_width > 0.0f) {
            float by_width = viewport_width / width;
            if (by_width < scale) scale = by_width;
        }
        return scale;
    }
};

struct UiRect {
    float x{0.0f};
    float y{0.0f};
    float w{0.0f};
    float h{0.0f};
};

/// Four authored lengths, one per side: margin, padding, inset.
struct UiEdges {
    UiLength top{};
    UiLength right{};
    UiLength bottom{};
    UiLength left{};

    static constexpr UiEdges all(UiLength length) { return {length, length, length, length}; }
};

/// Four sides in pixels as layout resolved them, percentages included.
struct UiPixelEdges {
    float top{0.0f};
    float right{0.0f};
    float bottom{0.0f};
    float left{0.0f};
};

struct UiCorners {
    float top_left{0.0f};
    float top_right{0.0f};
    float bottom_right{0.0f};
    float bottom_left{0.0f};
};

struct UiBoxShadow {
    float offset_x{0.0f};
    float offset_y{0.0f};
    float blur{0.0f};
    float spread{0.0f};
    UiColor color{0.0f, 0.0f, 0.0f, 0.0f};

    [[nodiscard]] bool active() const { return color.a > 0.0f && (blur > 0.0f || spread > 0.0f); }
};

struct UiGradient {
    UiColor from{0.0f, 0.0f, 0.0f, 0.0f};
    UiColor to{0.0f, 0.0f, 0.0f, 0.0f};
    float angle{180.0f}; // degrees, 0=right, 90=down, 180=left, 270=up

    [[nodiscard]] bool active() const { return from.a > 0.0f || to.a > 0.0f; }
};

} // namespace fjell
