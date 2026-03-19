#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fjell {

class FontAtlas;

struct PositionedGlyph {
    float x, y;            // top-left position in screen pixels
    float w, h;            // glyph size in screen pixels
    float uv_x0, uv_y0;   // atlas UV top-left
    float uv_x1, uv_y1;   // atlas UV bottom-right
};

struct ShapedText {
    std::vector<PositionedGlyph> glyphs;
    float width;           // total text width
    float height;          // total text height
};

/// Takes a string + font + size + max width → positioned glyph list.
/// Handles word-wrap and basic kerning.
class TextShaper {
public:
    [[nodiscard]] static ShapedText shape(const std::string& text,
                                          const FontAtlas& font,
                                          float font_size,
                                          float max_width = 0.0f);
};

} // namespace fjell
