#include "ftk/ui/kit/feedback.hpp"

#include "ftk/ui/draw_list.hpp"
#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/item.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/text.hpp"

#include <algorithm>
#include <cstring>
#include <string>

namespace ftk::ui {

using namespace corekit;

namespace {

constexpr float EMPTY_ICON_SIZE = 28.0f;
// A callout's severity edge, the room inside it, its corners, and how much of
// the severity's colour tints it.
constexpr float CALLOUT_EDGE = 3.0f;
constexpr float CALLOUT_PAD_X = 10.0f;
constexpr float CALLOUT_PAD_Y = 8.0f;
constexpr float CALLOUT_ROUNDING = 4.0f;
constexpr float CALLOUT_TINT = 0.10f;
// The explanation wraps at about thirty characters' width, so it reads as
// a short paragraph under the title rather than one long line.
constexpr float EMPTY_TEXT_WIDTH = 220.0f;

// `text` as an item at the cursor, wrapped short of `right`, `drop` below
// the line's top as text sits level with a field.
core::Rect wrapped(std::string_view text, Color colour, float right, float drop = 0.0f) {
    const float width = std::max(right - cursor().x, 1.0f);
    const glm::vec2 size = measure(text, width);
    const core::Rect rect = ui().layout().next({size.x, size.y + drop * 2.0f});
    draw_text({rect.min.x, rect.min.y + drop}, colour, text, width);
    place_still(rect);
    return rect;
}

// Text wrapped at `wrap`, each line centred in `width` from `left`, the
// lines of the paragraph directly under each other.
void centred(std::string_view text, Color colour, float left, float width, float wrap) {
    Style style = kit().style;
    style.item_spacing.y = 0.0f;
    push_style(style);
    for (const text::Line& line : ui().text().break_lines(text, text_style(), wrap)) {
        std::string_view drawn = text.substr(line.begin, line.end - line.begin);
        while (!drawn.empty() && (drawn.back() == ' ' || drawn.back() == '\n')) drawn.remove_suffix(1);
        set_cursor({left + std::max((width - line.width) * 0.5f, 0.0f), cursor().y});
        ui::text(drawn, colour);
    }
    pop_style();
}

// Moves down by `gap` from the end of the last item.
void gap_below(float gap) {
    set_cursor({cursor().x, item_max().y + gap});
}

const char* glyph_of(Severity severity) {
    return severity == Severity::Success ? icon::success
         : severity == Severity::Warning ? icon::warning
                                         : icon::error;
}

Color colour_of(Severity severity) {
    return severity == Severity::Success ? theme::success()
         : severity == Severity::Warning ? theme::warning()
                                         : theme::error();
}

} // namespace

void status(Severity severity, const char* text) {
    const Color colour = colour_of(severity);
    FontScope small(Font::Current, theme::SMALL_TEXT);
    ui::text(glyph_of(severity), colour);
    same_line(theme::GAP_S + theme::GAP_XS);
    wrapped(text, colour, cursor().x + space_left().x);
}

bool status(Severity severity, const char* text, const char* action_icon, const char* action) {
    const std::string label = std::string(action_icon) + "  " + action;
    const float button_width = measure(label).x + kit().style.frame_padding.x * 2.0f;
    const glm::vec2 start = cursor();
    const float right = start.x + space_left().x;

    float text_bottom = 0.0f;
    {
        // Level with the button's text, as a line of fields is.
        FontScope small(Font::Current, theme::SMALL_TEXT);
        const Color colour = colour_of(severity);
        align_text_to_frame();
        ui::text(glyph_of(severity), colour);
        same_line(theme::GAP_S + theme::GAP_XS);
        (void)wrapped(text, colour, right - button_width - theme::GAP_M, kit().style.frame_padding.y);
        text_bottom = cursor().y;
    }

    // On the line's first row, at its end.
    set_cursor({right - button_width, start.y});
    const bool clicked = ui::action(action_icon, action, ButtonKind::Ghost);
    set_cursor({start.x, std::max(text_bottom, cursor().y)});
    reserve({0.0f, 0.0f});
    return clicked;
}

bool callout(Severity severity, const char* title, const char* text, const char* action_icon,
             const char* action) {
    core::Context& context = ui();
    const Color colour = colour_of(severity);
    const char* glyph = glyph_of(severity);
    const glm::vec2 origin = cursor();
    const float width = space_left().x;
    const float right = origin.x + width - CALLOUT_PAD_X;

    // The contents first and the box under them, drawn once their height
    // is known.
    core::DrawList& dl = context.draw_list();
    const std::size_t contents = dl.mark();

    const float icon_x = origin.x + CALLOUT_EDGE + CALLOUT_PAD_X;
    set_cursor({icon_x, origin.y + CALLOUT_PAD_Y});
    ui::text(glyph, colour);
    set_cursor({icon_x + measure(glyph).x + theme::GAP_M, origin.y + CALLOUT_PAD_Y});

    bool clicked = false;
    core::Layout& layout = context.layout();
    layout.begin_group();
    {
        FontScope bold(Font::Bold, 0.0f);
        (void)wrapped(title, corekit::text_colour(), right);
    }
    if (text != nullptr && *text != '\0') (void)wrapped(text, theme::text_secondary(), right);
    if (action != nullptr) {
        reserve({0.0f, theme::GAP_XS});
        clicked = action_icon != nullptr ? ui::action(action_icon, action) : button(action);
    }
    const float bottom = layout.end_group().max.y + CALLOUT_PAD_Y;

    const std::size_t box = dl.mark();
    Color tint = colour;
    tint.a *= CALLOUT_TINT;
    dl.rect_filled(origin, {origin.x + width, bottom}, faded(tint), CALLOUT_ROUNDING);
    dl.rect_filled(origin, {origin.x + CALLOUT_EDGE, bottom}, faded(colour), CALLOUT_ROUNDING, RoundCorners::Left);
    dl.put_under(contents, box);

    // The box as one item, so the layout continues under it.
    set_cursor(origin);
    reserve({width, bottom - origin.y});
    return clicked;
}

bool empty_state(const char* icon, const char* title, const char* what_to_do, const char* action) {
    const glm::vec2 origin = cursor();
    const glm::vec2 avail = space_left();
    const float wrap = std::min(EMPTY_TEXT_WIDTH, avail.x);
    const float gap = theme::GAP_M;

    // The height of the whole block, to centre it.
    float icon_h = 0.0f;
    {
        FontScope big(Font::Current, EMPTY_ICON_SIZE);
        icon_h = line_height();
    }
    const float title_h = measure(title, wrap).y;
    float text_h = 0.0f;
    {
        FontScope small(Font::Current, theme::SMALL_TEXT);
        text_h = measure(what_to_do, wrap).y;
    }
    float height = icon_h + gap + title_h + gap + text_h;
    if (action != nullptr) height += gap + frame_height();

    set_cursor({origin.x, origin.y + std::max((avail.y - height) * 0.5f, 0.0f)});
    {
        FontScope big(Font::Current, EMPTY_ICON_SIZE);
        centred(icon, theme::text_disabled(), origin.x, avail.x, wrap);
    }
    gap_below(gap);
    centred(title, corekit::text_colour(), origin.x, avail.x, wrap);
    gap_below(gap);
    {
        FontScope small(Font::Current, theme::SMALL_TEXT);
        centred(what_to_do, theme::text_secondary(), origin.x, avail.x, wrap);
    }

    bool clicked = false;
    if (action != nullptr) {
        gap_below(gap);
        const float w = measure(action).x + kit().style.frame_padding.x * 2.0f;
        set_cursor({origin.x + std::max((avail.x - w) * 0.5f, 0.0f), cursor().y});
        clicked = button(action);
    }
    return clicked;
}

void unsaved_dot(DrawList list, glm::vec2 centre) {
    list.circle_filled(centre, UNSAVED_DOT_RADIUS, theme::text());
}

} // namespace ftk::ui
