#include "ftk/ui/kit/picture_row.hpp"

#include "ftk/ui/draw_list.hpp"
#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/search.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/tooltip.hpp"
#include "ftk/ui/theme.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace ftk::ui {

using namespace corekit;

namespace {

// The picture, the room above and below it and at the row's sides, the gap
// to the text and between its lines, for each size.
struct Measures {
    glm::vec2 picture;
    float pad;
    float pad_side;
    float text_gap;
    float line_gap;
};
constexpr Measures SMALL{.picture = {64.0f, 36.0f}, .pad = 5.0f, .pad_side = 6.0f, .text_gap = 10.0f, .line_gap = 0.0f};
constexpr Measures LARGE{.picture = {128.0f, 72.0f}, .pad = 6.0f, .pad_side = 8.0f, .text_gap = 12.0f, .line_gap = 2.0f};

constexpr float ROUNDING = 4.0f;
constexpr float PICTURE_ROUNDING = 3.0f;
// How much of an unavailable row's picture shows through, in greys.
constexpr float UNAVAILABLE_PICTURE = 0.4f;
// The size of the lines under the name.
constexpr float SMALL_LINE = theme::SMALL_TEXT - 1.0f;

float line_height_in(Font font, float size) {
    FontScope scope(font, size);
    return line_height();
}

} // namespace

PictureRowResult picture_row(const PictureRowSpec& spec, bool selected) {
    PictureRowResult result;
    core::Context& context = ui();
    const Measures& m = spec.size == PictureRowSize::large ? LARGE : SMALL;
    IdScope scope(spec.id);
    const glm::vec2 size{space_left().x, m.picture.y + (m.pad * 2.0f)};
    const core::Rect row = context.layout().next(size);
    const Id row_id = context.id("##row");
    const core::Interaction in = place(row_id, row);
    context.semantics(row_id, core::Role::ListItem, spec.name, {},
                      {.disabled = spec.unavailable, .selected = selected});
    result.clicked = in.clicked;
    result.double_clicked = in.hovered && in.double_clicked;

    core::DrawList& dl = context.draw_list();
    if (selected) {
        dl.rect_filled(row.min, row.max, faded(theme::selection()), ROUNDING);
    } else if (in.hovered) {
        dl.rect_filled(row.min, row.max, faded(theme::surface_hover()), ROUNDING);
    }

    const glm::vec2 pic_min{row.min.x + m.pad_side, row.min.y + m.pad};
    const glm::vec2 pic_max = pic_min + m.picture;
    if (spec.picture) {
        dl.image(spec.picture, pic_min, pic_max, PICTURE_ROUNDING,
                 {.tint = faded({1.0f, 1.0f, 1.0f, spec.unavailable ? UNAVAILABLE_PICTURE : 1.0f}),
                  .grey = spec.unavailable});
    } else {
        dl.rect_filled(pic_min, pic_max, faded(theme::surface_sunken()), PICTURE_ROUNDING);
        if (spec.icon != nullptr) {
            const glm::vec2 glyph = measure(spec.icon);
            draw_text({std::floor(pic_min.x + ((m.picture.x - glyph.x) * 0.5f)),
                       std::floor(pic_min.y + ((m.picture.y - glyph.y) * 0.5f))},
                      spec.unavailable ? theme::text_disabled() : theme::text_secondary(), spec.icon);
        }
    }

    // The name, its path and the last line, the block centred on the picture.
    const float text_x = pic_max.x + m.text_gap;
    const float text_w = std::max(row.max.x - m.pad_side - text_x, 0.0f);
    const std::string_view last = spec.problem.empty() ? spec.detail : spec.problem;
    const float name_h = line_height();
    const float path_h = line_height_in(theme::mono_font(), SMALL_LINE);
    const float last_h = line_height_in(Font::Current, SMALL_LINE);
    const float block = name_h + (spec.path.empty() ? 0.0f : m.line_gap + path_h) + (last.empty() ? 0.0f : m.line_gap + last_h);
    float y = std::floor(row.min.y + ((size.y - block) * 0.5f));

    const DrawList list = DrawList::window();
    detail::draw_highlighted(list, {text_x, y}, ellipsized(spec.name, text_w), spec.query,
                             faded(spec.unavailable ? theme::text_secondary() : theme::text()));
    y += name_h;
    if (!spec.path.empty()) {
        FontScope mono(theme::mono_font(), SMALL_LINE);
        y += m.line_gap;
        detail::draw_highlighted(list, {text_x, y}, ellipsized(spec.path, text_w), spec.query,
                                 faded(spec.unavailable ? theme::text_disabled() : theme::text_secondary()));
        y += path_h;
    }
    if (!last.empty()) {
        FontScope small(Font::Current, SMALL_LINE);
        y += m.line_gap;
        if (spec.problem.empty()) {
            draw_text({text_x, y}, theme::text_secondary(), ellipsized(spec.detail, text_w));
        } else {
            const float indent = measure(icon::error).x + theme::GAP_S;
            draw_text({text_x, y}, theme::error(), icon::error);
            draw_text({text_x + indent, y}, theme::error(), ellipsized(spec.problem, std::max(text_w - indent, 0.0f)));
        }
    }

    // The row stays the item just drawn, for the owner's menu.
    kit().item = {.id = row_id, .rect = row, .takes = true, .in = in};
    if (in.hovered && !context.dragging() && !spec.tooltip.empty()) item_tooltip(spec.tooltip);
    return result;
}

} // namespace ftk::ui
