#pragma once

#include <cstdint>

namespace fjell {

enum class UiUnit : uint8_t {
    px,      ///< absolute pixels
    percent, ///< fraction of the containing box's corresponding axis
    auto_,   ///< sized by content or flex, or "not set" where no auto keyword exists
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

    /// The value when it is in pixels; `fallback` for a percentage or auto,
    /// which only layout can turn into pixels.
    [[nodiscard]] constexpr float px_or(float fallback) const {
        return unit == UiUnit::px ? value : fallback;
    }

    friend constexpr bool operator==(const UiLength&, const UiLength&) = default;
};

} // namespace fjell
