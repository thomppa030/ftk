#include "ui/game_ui/text_shaper.hpp"
#include "ui/game_ui/font_atlas.hpp"

#include <algorithm>
#include <cmath>

namespace fjell {

static constexpr float TAB_STOP_SPACES = 4.0f;

ShapedText TextShaper::shape(const std::string& text, const FontAtlas& font,
                             float font_size, float max_width,
                             float line_height_override) {
    ShapedText result{};
    float scale = font_size;
    float cursor_x = 0.0f;
    float baseline_y = font.metrics().ascender * scale;
    float line_height = (line_height_override > 0.0f)
        ? line_height_override
        : font.metrics().line_height * scale;
    float max_x = 0.0f;

    size_t word_start_glyph = 0;
    float word_start_x = 0.0f;
    uint32_t prev_cp = 0;

    // Get space advance for tab stops
    const auto* space_gi = font.glyph(' ');
    float space_adv = space_gi ? space_gi->advance * scale : font_size * 0.25f;

    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];

        // Carriage return: skip (handle \r\n as just \n)
        if (c == '\r') continue;

        if (c == '\n') {
            cursor_x = 0.0f;
            baseline_y += line_height;
            word_start_glyph = result.glyphs.size();
            word_start_x = 0.0f;
            prev_cp = 0;
            continue;
        }

        if (c == '\t') {
            float tab_width = space_adv * TAB_STOP_SPACES;
            cursor_x = std::ceil(cursor_x / tab_width + 1.0f) * tab_width;
            max_x = std::max(max_x, cursor_x);
            word_start_glyph = result.glyphs.size();
            word_start_x = cursor_x;
            prev_cp = 0;
            continue;
        }

        auto cp = static_cast<uint32_t>(static_cast<unsigned char>(c));
        const auto* gi = font.glyph(cp);
        if (!gi) {
            gi = font.glyph('?');
            if (!gi) continue;
        }

        // Kerning adjustment
        if (prev_cp != 0) {
            cursor_x += font.kerning(prev_cp, cp) * scale;
        }

        float adv = gi->advance * scale;

        // Space: advance cursor, track word boundary, don't emit glyph
        if (c == ' ') {
            cursor_x += adv;
            max_x = std::max(max_x, cursor_x);
            word_start_glyph = result.glyphs.size();
            word_start_x = cursor_x;
            prev_cp = cp;
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
            prev_cp = 0;
        }

        // Emit glyph quad
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
        prev_cp = cp;
    }

    result.width = max_x;
    result.height = baseline_y - font.metrics().descender * scale;
    return result;
}

} // namespace fjell
