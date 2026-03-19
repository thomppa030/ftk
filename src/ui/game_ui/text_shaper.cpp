#include "ui/game_ui/text_shaper.hpp"
#include "ui/game_ui/font_atlas.hpp"

#include <algorithm>

namespace fjell {

ShapedText TextShaper::shape(const std::string& text, const FontAtlas& font,
                             float font_size, float max_width) {
    ShapedText result{};
    float scale = font_size; // em-normalized × font_size = pixels
    float cursor_x = 0.0f;
    // cursor_y tracks the baseline position from the top of the text box.
    // First baseline = ascender distance from top.
    float baseline_y = font.metrics().ascender * scale;
    float line_height = font.metrics().line_height * scale;
    float max_x = 0.0f;

    size_t word_start_glyph = 0;
    float word_start_x = 0.0f;

    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];

        if (c == '\n') {
            cursor_x = 0.0f;
            baseline_y += line_height;
            word_start_glyph = result.glyphs.size();
            word_start_x = 0.0f;
            continue;
        }

        auto cp = static_cast<uint32_t>(static_cast<unsigned char>(c));
        const auto* gi = font.glyph(cp);
        if (!gi) {
            gi = font.glyph('?');
            if (!gi) continue;
        }

        float adv = gi->advance * scale;

        // Space: advance cursor, track word boundary, don't emit glyph
        if (c == ' ') {
            cursor_x += adv;
            max_x = std::max(max_x, cursor_x);
            word_start_glyph = result.glyphs.size();
            word_start_x = cursor_x;
            continue;
        }

        // Word wrap
        if (max_width > 0.0f && cursor_x + adv > max_width) {
            if (word_start_glyph < result.glyphs.size()) {
                float dx = word_start_x;
                baseline_y += line_height;
                for (size_t j = word_start_glyph; j < result.glyphs.size(); ++j) {
                    result.glyphs[j].x -= dx;
                    result.glyphs[j].y += line_height;
                }
                cursor_x -= dx;
            } else {
                cursor_x = 0.0f;
                baseline_y += line_height;
            }
            word_start_glyph = result.glyphs.size();
            word_start_x = 0.0f;
        }

        // Emit glyph quad covering the full MSDF cell.
        // quad_x: left edge relative to cursor (em, includes SDF padding)
        // quad_y: top edge relative to baseline (em, Y-up, positive = above baseline)
        // Screen Y is Y-down, so quad top in screen = baseline_y - quad_y * scale
        if (gi->quad_w > 0 && gi->quad_h > 0) {
            PositionedGlyph pg{};
            pg.x = cursor_x + gi->quad_x * scale;
            pg.y = baseline_y - gi->quad_y * scale;
            pg.w = gi->quad_w * scale;
            pg.h = gi->quad_h * scale;
            pg.uv_x0 = gi->uv_x0;
            pg.uv_y0 = gi->uv_y0;
            pg.uv_x1 = gi->uv_x1;
            pg.uv_y1 = gi->uv_y1;
            result.glyphs.push_back(pg);
        }

        cursor_x += adv;
        max_x = std::max(max_x, cursor_x);
    }

    result.width = max_x;
    result.height = baseline_y - font.metrics().descender * scale;
    return result;
}

} // namespace fjell
