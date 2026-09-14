#pragma once

#include <cstdint>
#include <string>

namespace fjell {

enum class UiUnit : uint8_t {
    px,      ///< absolute pixels
    percent, ///< fraction of the containing box's corresponding axis
    auto_,   ///< sized by content or flex, or "not set" where no auto keyword exists
    em,      ///< multiples of the node's own font size (the parent's, for font-size itself)
    rem,     ///< multiples of the root node's font size
    vw,      ///< hundredths of the viewport width
    vh,      ///< hundredths of the viewport height
    vmin,    ///< hundredths of the smaller viewport side
    vmax,    ///< hundredths of the larger viewport side
};

/// A CSS length: a number plus the unit that gives it meaning.
///
/// `auto_` doubles as "not set" on properties that have no auto keyword —
/// the min/max bounds, padding, gaps — where it leaves the layout engine
/// at its own default. Percentages only resolve during layout; anything
/// that needs pixels before then reads the resolved values the layout
/// wrote back onto the node.
struct UiLength {
    float value{0.0f};
    UiUnit unit{UiUnit::px};

    static constexpr UiLength pixels(float v) { return {v, UiUnit::px}; }
    static constexpr UiLength percent(float v) { return {v, UiUnit::percent}; }
    static constexpr UiLength automatic() { return {0.0f, UiUnit::auto_}; }

    [[nodiscard]] constexpr bool is_auto() const { return unit == UiUnit::auto_; }
    [[nodiscard]] constexpr bool is_percent() const { return unit == UiUnit::percent; }
    /// Font- or viewport-relative: needs a UiLengthContext to become pixels.
    [[nodiscard]] constexpr bool is_relative() const {
        return unit != UiUnit::px && unit != UiUnit::percent && unit != UiUnit::auto_;
    }

    /// The value when it is in pixels; `fallback` for a percentage or auto,
    /// which only layout can turn into pixels.
    [[nodiscard]] constexpr float px_or(float fallback) const {
        return unit == UiUnit::px ? value : fallback;
    }

    friend constexpr bool operator==(const UiLength&, const UiLength&) = default;
};

/// What the relative units measure against.
struct UiLengthContext {
    float font_size{16.0f};      ///< the node's own computed font size, for em
    float root_font_size{16.0f}; ///< the root node's, for rem
    float viewport_width{0.0f};
    float viewport_height{0.0f};
};

/// Turns a font- or viewport-relative length into pixels. Pixels, percent
/// and auto pass through unchanged: the layout engine handles those.
[[nodiscard]] constexpr UiLength resolve_relative(UiLength length, const UiLengthContext& ctx) {
    float vmin = ctx.viewport_width < ctx.viewport_height ? ctx.viewport_width : ctx.viewport_height;
    float vmax = ctx.viewport_width < ctx.viewport_height ? ctx.viewport_height : ctx.viewport_width;
    switch (length.unit) {
    case UiUnit::em: return UiLength::pixels(length.value * ctx.font_size);
    case UiUnit::rem: return UiLength::pixels(length.value * ctx.root_font_size);
    case UiUnit::vw: return UiLength::pixels(length.value * ctx.viewport_width / 100.0f);
    case UiUnit::vh: return UiLength::pixels(length.value * ctx.viewport_height / 100.0f);
    case UiUnit::vmin: return UiLength::pixels(length.value * vmin / 100.0f);
    case UiUnit::vmax: return UiLength::pixels(length.value * vmax / 100.0f);
    case UiUnit::px:
    case UiUnit::percent:
    case UiUnit::auto_:
        return length;
    }
    return length;
}

/// The suffix a unit is written with; empty for pixels, which the sheets
/// author as bare numbers.
[[nodiscard]] constexpr const char* unit_suffix(UiUnit unit) {
    switch (unit) {
    case UiUnit::px: return "";
    case UiUnit::percent: return "%";
    case UiUnit::auto_: return "";
    case UiUnit::em: return "em";
    case UiUnit::rem: return "rem";
    case UiUnit::vw: return "vw";
    case UiUnit::vh: return "vh";
    case UiUnit::vmin: return "vmin";
    case UiUnit::vmax: return "vmax";
    }
    return "";
}

/// The length as it is written in a stylesheet: `auto`, `50%`, `1.5em`,
/// or a bare number for pixels.
[[nodiscard]] std::string to_css(UiLength length);

} // namespace fjell
