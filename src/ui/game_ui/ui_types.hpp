#pragma once

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

struct UiRect {
    float x{0.0f};
    float y{0.0f};
    float w{0.0f};
    float h{0.0f};
};

struct UiEdges {
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
