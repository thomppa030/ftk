#include "ftk/ui/kit/overlay.hpp"

#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/text.hpp"

#include <cmath>

namespace ftk::ui {

using namespace corekit;

namespace {
constexpr float PLATE_ALPHA = 0.85f;
constexpr float PLATE_HOVERED_ALPHA = 0.94f;
constexpr float PAD_X = 8.0f;
constexpr float PAD_Y = 3.0f;
constexpr float GAP = 6.0f;       // between the icon and the words, and between pieces
constexpr float KEY_PAD_X = 5.0f;
constexpr float KEY_PAD_Y = 1.0f;
constexpr float KEY_LIP = 2.0f;

float piece_width(const OverlayPiece& piece) {
    if (!piece.key) return measure(piece.text).x;
    FontScope bold(Font::Bold, 0.0f);
    return measure(piece.text).x + KEY_PAD_X * 2.0f;
}

// A key as a small raised cap with a darker lip along its bottom, the name
// in the bold face (sheet 26, sized to a line of text).
void draw_key(DrawList& list, glm::vec2 pos, float width, std::string_view name) {
    const float h = font_size() + KEY_PAD_Y * 2.0f;
    const glm::vec2 min{pos.x, pos.y - KEY_PAD_Y};
    const glm::vec2 max{pos.x + width, min.y + h};
    const float rounding = kit().style.frame_rounding - 1.0f;
    list.rect_filled(min, max, theme::surface_raised(), rounding);
    list.rect_filled({min.x, max.y - KEY_LIP}, max, theme::border(), rounding, RoundCorners::Bottom);
    FontScope bold(Font::Bold, 0.0f);
    list.text({pos.x + KEY_PAD_X, pos.y}, theme::text(), name);
}
} // namespace

void overlay_plate(DrawList list, glm::vec2 min, glm::vec2 max) {
    list.rect_filled(min, max, overlay_plate_colour(), kit().style.frame_rounding);
}

void overlay_tag(DrawList list, glm::vec2 at, std::string_view text, std::string_view detail) {
    constexpr float TAG_PAD_X = 7.0f;
    constexpr float TAG_PAD_Y = 2.0f;
    const glm::vec2 text_size = measure(text);
    const float detail_width = detail.empty() ? 0.0f : GAP + measure(detail).x;
    const glm::vec2 min{std::floor(at.x), std::floor(at.y - text_size.y * 0.5f - TAG_PAD_Y)};
    const glm::vec2 max{min.x + TAG_PAD_X * 2.0f + text_size.x + detail_width, min.y + text_size.y + TAG_PAD_Y * 2.0f};
    overlay_plate(list, min, max);
    list.text({min.x + TAG_PAD_X, min.y + TAG_PAD_Y}, theme::text(), text);
    if (!detail.empty()) {
        list.text({min.x + TAG_PAD_X + text_size.x + GAP, min.y + TAG_PAD_Y}, theme::text_secondary(), detail);
    }
}

Color overlay_plate_colour() {
    Color plate = theme::surface_sunken();
    plate.a = PLATE_ALPHA;
    return plate;
}

Color overlay_plate_hovered_colour() {
    Color plate = theme::surface_active();
    plate.a = PLATE_HOVERED_ALPHA;
    return plate;
}

glm::vec2 corner_note_size(const CornerNoteSpec& spec) {
    // Keys stand a little taller than the words, so a line holding one gets
    // their height.
    bool has_key = false;
    float line_w = 0.0f;
    if (spec.icon != nullptr) line_w += measure(spec.icon).x + GAP;
    for (std::size_t i = 0; i < spec.line.size(); ++i) {
        line_w += piece_width(spec.line[i]) + (i > 0 ? GAP : 0.0f);
        has_key |= spec.line[i].key;
    }
    const float pad_y = has_key ? PAD_Y + KEY_PAD_Y : PAD_Y;
    return {std::floor(line_w + PAD_X * 2.0f), font_size() + pad_y * 2.0f};
}

glm::vec2 corner_note(DrawList list, glm::vec2 at, Corner corner, const CornerNoteSpec& spec) {
    const glm::vec2 size = corner_note_size(spec);
    bool has_key = false;
    for (const OverlayPiece& piece : spec.line) has_key |= piece.key;
    const float pad_y = has_key ? PAD_Y + KEY_PAD_Y : PAD_Y;

    const bool right = corner == Corner::TopRight || corner == Corner::BottomRight;
    const bool bottom = corner == Corner::BottomLeft || corner == Corner::BottomRight;
    const glm::vec2 min{std::floor(right ? at.x - size.x : at.x), std::floor(bottom ? at.y - size.y : at.y)};
    overlay_plate(list, min, min + size);

    float x = min.x + PAD_X;
    const float y = min.y + pad_y;
    if (spec.icon != nullptr) {
        list.text({x, y}, spec.icon_ink ? *spec.icon_ink : spec.ink, spec.icon);
        x += measure(spec.icon).x + GAP;
    }
    for (const OverlayPiece& piece : spec.line) {
        const float w = piece_width(piece);
        if (piece.key) {
            draw_key(list, {x, y}, w, piece.text);
        } else {
            list.text({x, y}, spec.ink, piece.text);
        }
        x += w + GAP;
    }
    return size;
}

} // namespace ftk::ui
