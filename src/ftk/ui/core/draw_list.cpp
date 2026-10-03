#include "ftk/ui/core/draw_list.hpp"

#include "ftk/base/log.hpp"
#include "ftk/ui/core/text.hpp"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace ftk::ui::core {

namespace {

// The radii (top-left, top-right, bottom-right, bottom-left) that round
// `corners` by `rounding`, no more than half the rectangle's shorter side.
glm::vec4 radii(glm::vec2 min, glm::vec2 max, float rounding, RoundCorners corners) {
    const float r = std::clamp(rounding, 0.0f, 0.5f * std::min(max.x - min.x, max.y - min.y));
    switch (corners) {
    case RoundCorners::All: return {r, r, r, r};
    case RoundCorners::Top: return {r, r, 0.0f, 0.0f};
    case RoundCorners::Bottom: return {0.0f, 0.0f, r, r};
    case RoundCorners::Left: return {r, 0.0f, 0.0f, r};
    case RoundCorners::Right: return {0.0f, r, r, 0.0f};
    case RoundCorners::TopLeft: return {r, 0.0f, 0.0f, 0.0f};
    case RoundCorners::TopRight: return {0.0f, r, 0.0f, 0.0f};
    case RoundCorners::BottomLeft: return {0.0f, 0.0f, 0.0f, r};
    case RoundCorners::BottomRight: return {0.0f, 0.0f, r, 0.0f};
    case RoundCorners::None: return {0.0f, 0.0f, 0.0f, 0.0f};
    }
    return {0.0f, 0.0f, 0.0f, 0.0f};
}

uint32_t byte(float value) {
    return static_cast<uint32_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

} // namespace

uint32_t pack_colour(Color colour) {
    return byte(colour.r) | byte(colour.g) << 8 | byte(colour.b) << 16 | byte(colour.a) << 24;
}

void DrawList::reset(DrawData& data, glm::vec2 size, float scale, TextSystem* text) {
    data_ = &data;
    text_ = text;
    scale_ = scale;
    data.primitives.clear();
    data.clips.assign(1, {.min = {0.0f, 0.0f}, .max = size});
    data.textures.assign(1, Texture{});
    clip_stack_.assign(1, 0);
}

void DrawList::add(Primitive p) {
    const ClipRect& clip = data_->clips[clip_stack_.back()];
    // Wholly outside the clip, it would draw nothing.
    if (p.max.x <= clip.min.x || p.max.y <= clip.min.y || p.min.x >= clip.max.x || p.min.y >= clip.max.y) return;
    if ((p.colour >> 24) == 0 && (p.colour2 >> 24) == 0) return;
    p.clip = clip_stack_.back();
    data_->primitives.push_back(p);
}

void DrawList::rect_filled(glm::vec2 min, glm::vec2 max, Color colour, float rounding, RoundCorners corners) {
    const uint32_t packed = pack_colour(colour);
    add({.min = min, .max = max, .shape = radii(min, max, rounding, corners), .colour = packed, .colour2 = packed});
}

void DrawList::rect(glm::vec2 min, glm::vec2 max, Color colour, float rounding, RoundCorners corners,
                    float thickness) {
    const uint32_t packed = pack_colour(colour);
    add({.min = min,
         .max = max,
         .shape = radii(min, max, rounding, corners),
         .colour = packed,
         .colour2 = packed,
         .thickness = thickness});
}

void DrawList::rect_gradient(glm::vec2 min, glm::vec2 max, Color left, Color right) {
    add({.min = min, .max = max, .colour = pack_colour(left), .colour2 = pack_colour(right)});
}

void DrawList::shadow(glm::vec2 min, glm::vec2 max, Color colour, float rounding, float softness) {
    const uint32_t packed = pack_colour(colour);
    add({.min = min,
         .max = max,
         .shape = radii(min, max, rounding, RoundCorners::All),
         .colour = packed,
         .colour2 = packed,
         .softness = softness});
}

void DrawList::segment(glm::vec2 from, glm::vec2 to, uint32_t colour, float thickness) {
    const float half = 0.5f * thickness;
    add({.min = glm::min(from, to) - half,
         .max = glm::max(from, to) + half,
         .shape = {from, to},
         .colour = colour,
         .colour2 = colour,
         .thickness = thickness,
         .kind = PrimitiveKind::Segment});
}

void DrawList::line(glm::vec2 from, glm::vec2 to, Color colour, float thickness) {
    segment(from, to, pack_colour(colour), thickness);
}

void DrawList::polyline(std::span<const glm::vec2> points, Color colour, float thickness, bool closed) {
    if (points.size() < 2) return;
    const uint32_t packed = pack_colour(colour);
    for (std::size_t i = 0; i + 1 < points.size(); ++i) segment(points[i], points[i + 1], packed, thickness);
    if (closed && points.size() > 2) segment(points.back(), points.front(), packed, thickness);
}

void DrawList::circle_filled(glm::vec2 centre, float radius, Color colour) {
    rect_filled(centre - radius, centre + radius, colour, radius);
}

void DrawList::circle(glm::vec2 centre, float radius, Color colour, float thickness) {
    // The outline is drawn inside the shape's edge, so the edge goes out
    // by half the width to centre the line on the radius.
    const float outer = radius + 0.5f * thickness;
    rect(centre - outer, centre + outer, colour, outer, RoundCorners::All, thickness);
}

void DrawList::arc(glm::vec2 centre, float radius, float from, float to, Color colour, float thickness) {
    const uint32_t packed = pack_colour(colour);
    const float outer = radius + 0.5f * thickness;
    add({.min = centre - outer,
         .max = centre + outer,
         .shape = {centre, radius, 0.0f},
         .extra = {from, to, 0.0f, 0.0f},
         .colour = packed,
         .colour2 = packed,
         .thickness = thickness,
         .kind = PrimitiveKind::Arc});
}

void DrawList::triangle_filled(glm::vec2 a, glm::vec2 b, glm::vec2 c, Color colour) {
    const uint32_t packed = pack_colour(colour);
    add({.min = glm::min(a, glm::min(b, c)),
         .max = glm::max(a, glm::max(b, c)),
         .shape = {a, b},
         .extra = {c, 0.0f, 0.0f},
         .colour = packed,
         .colour2 = packed,
         .kind = PrimitiveKind::Triangle});
}

void DrawList::triangle(glm::vec2 a, glm::vec2 b, glm::vec2 c, Color colour, float thickness) {
    const glm::vec2 corners[] = {a, b, c};
    polyline(corners, colour, thickness, true);
}

void DrawList::quad_filled(glm::vec2 a, glm::vec2 b, glm::vec2 c, glm::vec2 d, Color colour) {
    triangle_filled(a, b, c, colour);
    triangle_filled(a, c, d, colour);
}

void DrawList::quad(glm::vec2 a, glm::vec2 b, glm::vec2 c, glm::vec2 d, Color colour, float thickness) {
    const glm::vec2 corners[] = {a, b, c, d};
    polyline(corners, colour, thickness, true);
}

void DrawList::text(glm::vec2 at, Color colour, std::string_view text) {
    if (text_ != nullptr) text_->draw(*this, at, colour, text, text_->style());
}

void DrawList::text_wrapped(glm::vec2 at, Color colour, std::string_view text, float wrap_width) {
    if (text_ != nullptr) text_->draw(*this, at, colour, text, text_->style(), wrap_width);
}

void DrawList::glyph(glm::vec2 min, glm::vec2 max, glm::vec2 uv_min, glm::vec2 uv_max, uint32_t page, Color colour) {
    const uint32_t packed = pack_colour(colour);
    add({.min = min,
         .max = max,
         .extra = {uv_min, uv_max},
         .colour = packed,
         .colour2 = packed,
         .kind = PrimitiveKind::Glyph,
         .texture = page});
}

void DrawList::image(Texture texture, glm::vec2 min, glm::vec2 max, float rounding, glm::vec2 uv_min,
                     glm::vec2 uv_max) {
    if (!texture) return;
    // Each picture once per frame, however often it is shown.
    auto& textures = data_->textures;
    auto found = std::find(textures.begin() + 1, textures.end(), texture);
    if (found == textures.end()) {
        textures.push_back(texture);
        found = textures.end() - 1;
    }
    const uint32_t white = pack_colour({1.0f, 1.0f, 1.0f, 1.0f});
    add({.min = min,
         .max = max,
         .shape = radii(min, max, rounding, RoundCorners::All),
         .extra = {uv_min, uv_max},
         .colour = white,
         .colour2 = white,
         .texture = static_cast<uint32_t>(found - textures.begin())});
}

void DrawList::push_clip(glm::vec2 min, glm::vec2 max) {
    const ClipRect& outer = data_->clips[clip_stack_.back()];
    ClipRect clip{.min = glm::max(min, outer.min), .max = glm::min(max, outer.max)};
    clip.max = glm::max(clip.max, clip.min);
    // The same rectangle as the last made reuses its index.
    const ClipRect& last = data_->clips.back();
    if (last.min == clip.min && last.max == clip.max) {
        clip_stack_.push_back(static_cast<uint32_t>(data_->clips.size() - 1));
        return;
    }
    data_->clips.push_back(clip);
    clip_stack_.push_back(static_cast<uint32_t>(data_->clips.size() - 1));
}

void DrawList::pop_clip() {
    if (clip_stack_.size() <= 1) {
        FTK_CORE_ERROR("UI: a clip popped that was never pushed");
        return;
    }
    clip_stack_.pop_back();
}

glm::vec2 DrawList::clip_min() const {
    return data_->clips[clip_stack_.back()].min;
}

glm::vec2 DrawList::clip_max() const {
    return data_->clips[clip_stack_.back()].max;
}

} // namespace ftk::ui::core
