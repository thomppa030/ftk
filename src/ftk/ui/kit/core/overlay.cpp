#include "ftk/ui/kit/overlay.hpp"

#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/theme.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

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
constexpr float SWATCH = 8.0f;     // a swatch's width, and the gap after it is SWATCH_GAP
constexpr float SWATCH_GAP = 4.0f;
constexpr float RAMP_W = 64.0f;    // a ramp piece's width, and its height
constexpr float RAMP_H = 8.0f;
constexpr float LINE_GAP = 2.0f;   // between the lines of a wrapped note

float piece_width(const OverlayPiece& piece) {
    if (piece.ramp) return RAMP_W;
    if (!piece.key) return measure(piece.text).x + (piece.swatch ? SWATCH + SWATCH_GAP : 0.0f);
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

// The theme's heat ramp, `RAMP_W` wide, its middle on the line's; edged
// faintly, as its cold end is as dark as the plate.
void draw_ramp(DrawList& list, glm::vec2 pos) {
    const std::array<Color, 5> heat{theme::heat_none(), theme::heat_low(), theme::heat_mid(), theme::heat_high(),
                                    theme::heat_peak()};
    const float mid = pos.y + font_size() * 0.5f;
    const glm::vec2 min{pos.x, std::floor(mid - RAMP_H * 0.5f)};
    const glm::vec2 max{pos.x + RAMP_W, min.y + RAMP_H};
    for (std::size_t i = 0; i + 1 < heat.size(); ++i) {
        list.rect_gradient({min.x + theme::HEAT_STOPS[i] * RAMP_W, min.y},
                           {min.x + theme::HEAT_STOPS[i + 1] * RAMP_W, max.y}, heat[i], heat[i + 1]);
    }
    Color edge = theme::text();
    edge.a = 0.15f;
    list.rect(min, max, edge);
}

// Where each of a note's pieces goes, its line wrapped between pieces where
// it would pass the plate's widest: each piece's line and its x from the
// plate's left, inside its padding.
struct NoteLayout {
    std::vector<glm::vec2> at; // x, line
    float text_x{0.0f};        // where the words start, after the icon
    float width{0.0f};         // the widest line
    int lines{1};
    bool has_key{false};
};

NoteLayout layout_note(const CornerNoteSpec& spec) {
    NoteLayout out;
    if (spec.icon != nullptr) out.text_x = measure(spec.icon).x + GAP;
    const float limit = spec.max_width > 0.0f ? spec.max_width - PAD_X * 2.0f : std::numeric_limits<float>::infinity();
    float x = out.text_x;
    int line = 0;
    for (std::size_t i = 0; i < spec.line.size(); ++i) {
        const float w = piece_width(spec.line[i]);
        if (i > 0) {
            if (x + GAP + w > limit && x > out.text_x) {
                out.width = std::max(out.width, x);
                ++line;
                x = out.text_x;
            } else {
                x += GAP;
            }
        }
        out.at.push_back({x, static_cast<float>(line)});
        x += w;
        out.has_key |= spec.line[i].key;
    }
    out.width = std::max(out.width, x);
    out.lines = line + 1;
    return out;
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

namespace {

// A note's size from its layout. Keys stand a little taller than the words,
// so a note holding one gets their height.
glm::vec2 note_size(const NoteLayout& layout) {
    const float pad_y = layout.has_key ? PAD_Y + KEY_PAD_Y : PAD_Y;
    const float lines = static_cast<float>(layout.lines);
    return {std::floor(layout.width + PAD_X * 2.0f), font_size() * lines + LINE_GAP * (lines - 1.0f) + pad_y * 2.0f};
}

} // namespace

glm::vec2 corner_note_size(const CornerNoteSpec& spec) {
    return note_size(layout_note(spec));
}

glm::vec2 corner_note(DrawList list, glm::vec2 at, Corner corner, const CornerNoteSpec& spec) {
    const NoteLayout layout = layout_note(spec);
    const glm::vec2 size = note_size(layout);
    const float pad_y = layout.has_key ? PAD_Y + KEY_PAD_Y : PAD_Y;

    const bool right = corner == Corner::TopRight || corner == Corner::BottomRight;
    const bool bottom = corner == Corner::BottomLeft || corner == Corner::BottomRight;
    const glm::vec2 min{std::floor(right ? at.x - size.x : at.x), std::floor(bottom ? at.y - size.y : at.y)};
    overlay_plate(list, min, min + size);

    const glm::vec2 origin{min.x + PAD_X, min.y + pad_y};
    if (spec.icon != nullptr) list.text(origin, spec.icon_ink ? *spec.icon_ink : spec.ink, spec.icon);
    for (std::size_t i = 0; i < spec.line.size(); ++i) {
        const OverlayPiece& piece = spec.line[i];
        const float x = origin.x + layout.at[i].x;
        const float y = origin.y + layout.at[i].y * (font_size() + LINE_GAP);
        const Color ink = piece.ink ? *piece.ink : spec.ink;
        if (piece.ramp) {
            draw_ramp(list, {x, y});
        } else if (piece.key) {
            draw_key(list, {x, y}, piece_width(piece), piece.text);
        } else if (piece.swatch) {
            list.circle_filled({x + SWATCH * 0.5f, y + font_size() * 0.5f}, SWATCH * 0.5f, *piece.swatch);
            list.text({x + SWATCH + SWATCH_GAP, y}, ink, piece.text);
        } else {
            list.text({x, y}, ink, piece.text);
        }
    }
    return size;
}

} // namespace ftk::ui
