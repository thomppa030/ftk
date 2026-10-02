#pragma once

// Where a line of text may end, and shaped text filled into lines of a
// width. The breaks follow Unicode's line breaking algorithm (UAX #14) as
// far as Latin and CJK need it:
// - after spaces and hyphens, never inside a word or a number;
// - between ideographs and kana, but not before closing punctuation or
//   small kana, and not after opening brackets;
// - always after a line or paragraph separator.
//
//     const text::ShapedText shaped = text::shape(paragraph, chain, 14.0f);
//     for (const text::Line& line : text::break_lines(paragraph, shaped, 320.0f)) draw(shaped, line);

#include "ftk/text/shape.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace ftk::text {

enum class Break : uint8_t {
    /// No line may begin here.
    None,
    /// A line may begin here when the one before is full.
    Allowed,
    /// A line begins here: the one before ended at a newline.
    Mandatory,
};

/// For each byte of `text`, UTF-8, whether a line may begin there. Only a
/// byte that begins a character, past the first, is ever more than None.
[[nodiscard]] std::vector<Break> break_opportunities(std::string_view text);

struct Line {
    /// Its glyphs to draw, from `first_glyph` up to `end_glyph`: the spaces
    /// and newline it ends with, which hang past its end, are left out.
    std::size_t first_glyph{0};
    std::size_t end_glyph{0};
    /// Its bytes of the text, those it ends with included.
    std::size_t begin{0};
    std::size_t end{0};
    /// From its first glyph to the end of its last drawn one.
    float width{0.0f};
};

/// `shaped`, the shaping of `text`, filled into lines at most `width` wide,
/// broken where break_opportunities() allows. A word wider than a line is
/// broken between its characters.
[[nodiscard]] std::vector<Line> break_lines(std::string_view text, const ShapedText& shaped, float width);

} // namespace ftk::text
