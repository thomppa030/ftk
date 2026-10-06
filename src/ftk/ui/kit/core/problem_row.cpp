#include "ftk/ui/kit/problem_row.hpp"

#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/search.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/tooltip.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace ftk::ui {

using namespace corekit;

namespace {

// Room at the row's ends, the icon's column, the gap between the row's
// parts, and the corner.
constexpr float ROW_PAD = 6.0f;
constexpr float ICON_COLUMN = 18.0f;
constexpr float PART_GAP = 6.0f;
constexpr float ROUNDING = 3.0f;

Color severity_colour(Severity severity) {
    switch (severity) {
        case Severity::Error: return theme::error();
        case Severity::Warning: return theme::warning();
        case Severity::Success: break;
    }
    return theme::success();
}

const char* severity_icon(Severity severity) {
    switch (severity) {
        case Severity::Error: return icon::error;
        case Severity::Warning: return icon::warning;
        case Severity::Success: break;
    }
    return icon::success;
}

} // namespace

ProblemRowResult problem_row(const ProblemRowSpec& spec) {
    ProblemRowResult result;
    core::Context& context = ui();
    IdScope scope(spec.id);
    const Id row_id = context.id("##row");

    const float body = font_size();
    const float first_line = theme::TREE_ROW;
    float second_line = 0.0f;
    if (!spec.meanwhile.empty()) {
        FontScope small(Font::Current, theme::SMALL_TEXT);
        second_line = std::round(line_height());
    }
    const float height = first_line + second_line;
    const glm::vec2 min = cursor();
    const float width = std::max(space_left().x, 1.0f);
    const glm::vec2 max{min.x + width, min.y + height};

    // Out of sight a row keeps its room and draws nothing.
    if (can_skip_row(row_id, {.min = min, .max = max})) {
        place_still(context.layout().next({width, height}));
        return result;
    }

    const core::Rect row = context.layout().next({width, height});
    const core::Interaction in = place(row_id, row);
    context.semantics(row_id, core::Role::ListItem, spec.message, {}, {.selected = spec.selected});
    result.clicked = in.clicked;
    result.double_clicked = in.hovered && in.double_clicked;

    core::DrawList& dl = context.draw_list();
    if (spec.selected) {
        dl.rect_filled(min, max, faded(theme::selection()), ROUNDING);
    } else if (in.hovered) {
        dl.rect_filled(min, max, faded(theme::surface_hover()), ROUNDING);
    }

    const float text_y = std::floor(min.y + (first_line - body) * 0.5f);
    const char* glyph = severity_icon(spec.severity);
    draw_text({min.x + ROW_PAD + (ICON_COLUMN - measure(glyph).x) * 0.5f, text_y}, severity_colour(spec.severity),
              glyph);

    // Where, against the right edge, level with the message.
    float message_right = max.x - ROW_PAD;
    if (!spec.where.empty()) {
        const FontScope mono(theme::mono_font(), theme::SMALL_TEXT);
        const float where_w = measure(spec.where).x;
        draw_text({max.x - ROW_PAD - where_w, std::floor(text_y + (body - font_size()) * 0.5f)}, theme::text_disabled(),
                  spec.where);
        message_right -= where_w + PART_GAP * 2.0f;
    }

    const float x = min.x + ROW_PAD + ICON_COLUMN + PART_GAP;
    const float room = std::max(message_right - x, 1.0f);
    const bool cut = measure(spec.message).x > room;
    if (cut) {
        draw_text({x, text_y}, theme::text(), ellipsized(spec.message, room));
    } else {
        detail::draw_highlighted(DrawList::window(), {x, text_y}, spec.message, spec.query, theme::text());
    }
    if (!spec.meanwhile.empty()) {
        const FontScope small(Font::Current, theme::SMALL_TEXT);
        draw_text({x, text_y + body}, theme::text_secondary(),
                  ellipsized(spec.meanwhile, room));
    }

    if (in.hovered && !context.dragging()) {
        std::string tip;
        if (cut) tip = std::string(spec.message);
        if (!spec.tooltip.empty()) {
            if (!tip.empty()) tip += '\n';
            tip += spec.tooltip;
        }
        if (!tip.empty()) item_tooltip(tip);
    }
    return result;
}

} // namespace ftk::ui
