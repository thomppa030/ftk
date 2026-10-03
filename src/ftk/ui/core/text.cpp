#include "ftk/ui/core/text.hpp"

#include "ftk/base/log.hpp"
#include "ftk/ui/core/draw_list.hpp"

#include <algorithm>
#include <cmath>
#include <functional>

namespace ftk::ui::core {

namespace {

int32_t size_key(float size) {
    return static_cast<int32_t>(std::lround(size * 64.0f));
}

// The whole text as one line: what text without a newline is.
text::Line one_line(std::string_view text, const text::ShapedText& shaped) {
    return {.first_glyph = 0, .end_glyph = shaped.glyphs.size(), .begin = 0, .end = text.size(), .width = shaped.width};
}

} // namespace

std::size_t TextSystem::KeyHash::hash(const KeyView& key) {
    std::size_t hash = std::hash<std::string_view>{}(key.text);
    hash = hash * 1000003u ^ key.font;
    hash = hash * 1000003u ^ static_cast<uint32_t>(key.size);
    return hash;
}

TextSystem::TextSystem(text::AtlasOptions atlas) : atlas_{atlas} {}

FontId TextSystem::add_font(text::FontChain chain) {
    fonts_.push_back(std::move(chain));
    return static_cast<FontId>(fonts_.size() - 1);
}

void TextSystem::pop_style() {
    if (styles_.empty()) {
        FTK_CORE_ERROR("UI: a text style popped that was never pushed");
        return;
    }
    styles_.pop_back();
}

void TextSystem::begin_frame() {
    ++frame_;
    atlas_.begin_frame();
    // Kept while drawn: what neither this frame nor the last drew goes.
    std::erase_if(cache_, [&](const auto& entry) { return entry.second.last_used + 1 < frame_; });
}

const TextSystem::Shaped& TextSystem::shaped(std::string_view text, TextStyle style) {
    const KeyView key{.font = style.font, .size = size_key(style.size), .text = text};
    auto found = cache_.find(key);
    if (found == cache_.end()) {
        Shaped entry;
        entry.shaped = text::shape(text, fonts_[style.font], style.size);
        if (text.find('\n') == std::string_view::npos) {
            entry.lines.push_back(one_line(text, entry.shaped));
        } else {
            entry.lines = text::break_lines(text, entry.shaped, NO_WRAP);
        }
        found = cache_.emplace(Key{.font = key.font, .size = key.size, .text = std::string(text)}, std::move(entry)).first;
    }
    found->second.last_used = frame_;
    return found->second;
}

const std::vector<text::Line>& TextSystem::lines(std::string_view text, const Shaped& shaped, float wrap_width) {
    if (wrap_width == NO_WRAP) return shaped.lines;
    scratch_ = text::break_lines(text, shaped.shaped, wrap_width);
    return scratch_;
}

float TextSystem::line_height(TextStyle style) const {
    if (style.line_height > 0.0f) return style.line_height;
    return fonts_[style.font].primary().metrics(style.size).line_height();
}

glm::vec2 TextSystem::measure(std::string_view text, TextStyle style, float wrap_width) {
    if (style.font >= fonts_.size()) return {0.0f, 0.0f};
    if (text.empty()) return {0.0f, line_height(style)};
    const Shaped& entry = shaped(text, style);
    const std::vector<text::Line>& broken = lines(text, entry, wrap_width);
    float width = 0.0f;
    for (const text::Line& line : broken) width = std::max(width, line.width);
    const auto count = static_cast<float>(std::max<std::size_t>(broken.size(), 1));
    return {width, count * line_height(style)};
}

float TextSystem::caret_x(std::string_view text, TextStyle style, std::size_t at) {
    if (style.font >= fonts_.size() || text.empty()) return 0.0f;
    const text::ShapedText& shaped_text = shaped(text, style).shaped;
    // Before the first glyph drawn from that byte on; past the last, after
    // it.
    for (const text::Glyph& glyph : shaped_text.glyphs) {
        if (glyph.cluster >= at) return glyph.x;
    }
    return shaped_text.width;
}

std::size_t TextSystem::caret_at(std::string_view text, TextStyle style, float x) {
    if (style.font >= fonts_.size() || text.empty()) return 0;
    const text::ShapedText& shaped_text = shaped(text, style).shaped;
    std::size_t best = text.size();
    float best_distance = std::abs(x - shaped_text.width);
    for (const text::Glyph& glyph : shaped_text.glyphs) {
        const float distance = std::abs(x - glyph.x);
        if (distance < best_distance) {
            best_distance = distance;
            best = glyph.cluster;
        }
    }
    return best;
}

void TextSystem::draw(DrawList& list, glm::vec2 at, Color colour, std::string_view text, TextStyle style,
                      float wrap_width) {
    if (text.empty() || style.font >= fonts_.size()) return;
    const Shaped& entry = shaped(text, style);
    const text::Metrics metrics = fonts_[style.font].primary().metrics(style.size);
    const float scale = list.scale();
    const float pixel_size = style.size * scale;
    const float page_size = atlas_.page_size();

    // The face's line from the top, or its ascent and descent centred in
    // the line asked for.
    const float pitch = style.line_height > 0.0f ? style.line_height : metrics.line_height();
    const float top = style.line_height > 0.0f ? (pitch - metrics.ascent - metrics.descent) * 0.5f : 0.0f;
    float baseline = at.y + top + metrics.ascent;
    for (const text::Line& line : lines(text, entry, wrap_width)) {
        const float origin = line.first_glyph < entry.shaped.glyphs.size() ? entry.shaped.glyphs[line.first_glyph].x : 0.0f;
        for (std::size_t g = line.first_glyph; g < line.end_glyph; ++g) {
            const text::Glyph& glyph = entry.shaped.glyphs[g];
            // A control character draws nothing, whatever face took it.
            if (static_cast<unsigned char>(text[glyph.cluster]) < 0x20) continue;
            const auto placed = atlas_.glyph(*glyph.face, glyph.index, pixel_size);
            if (!placed || placed->rect.empty()) continue;
            // On whole pixels, as the atlas's coverage was drawn.
            const float x = std::round((at.x + glyph.x - origin + glyph.offset.x) * scale) + placed->bearing.x;
            const float y = std::round((baseline + glyph.offset.y) * scale) + placed->bearing.y;
            const glm::vec2 min = glm::vec2{x, y} / scale;
            const glm::vec2 size = glm::vec2{placed->rect.width, placed->rect.height} / scale;
            const glm::vec2 uv_min = glm::vec2{placed->rect.x, placed->rect.y} / page_size;
            const glm::vec2 uv_max = glm::vec2{placed->rect.x + placed->rect.width, placed->rect.y + placed->rect.height} / page_size;
            list.glyph(min, min + size, uv_min, uv_max, placed->page, colour);
        }
        baseline += pitch;
    }
}

} // namespace ftk::ui::core
