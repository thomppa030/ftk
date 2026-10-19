#include "ftk/ui/kit/status_bar.hpp"

#include "ftk/ui/draw_list.hpp"
#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/feedback.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/item.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/loading.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/state.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/tooltip.hpp"
#include "ftk/ui/kit/window.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace ftk::ui {

using namespace corekit;

namespace {

// A state dot's width.
constexpr float STATE_DOT = 7.0f;

// How wide the newest problem's sentence may grow before it is cut, and the
// space between a count's icon and its number.
constexpr float NEWEST_WIDTH = 280.0f;
constexpr float COUNT_GAP = theme::GAP_S;

Color with_alpha(Color colour, float alpha) {
    colour.a = alpha;
    return colour;
}

// The colour a tone reads in.
Color tone_colour(StatusTone tone) {
    switch (tone) {
        case StatusTone::Success: return theme::success();
        case StatusTone::Warning: return theme::warning();
        case StatusTone::Error: return theme::error();
        // The toggle colour is a wash; as text it reads whole.
        case StatusTone::On: return with_alpha(theme::toggle_on(), 1.0f);
        case StatusTone::Selected: return theme::accent();
        case StatusTone::Plain: break;
    }
    return theme::text_secondary();
}

// Where text `height` tall sits on the bar's line, centred in its height.
float line_y(float height) {
    return window_pos().y + (theme::STATUS_BAR - height) * 0.5f;
}

const char* const RIGHT_WIDTH = "##right_width";

} // namespace

StatusBar::StatusBar() {
    // Along the bottom of the work area, as wide as it.
    const WorkArea work = work_area();
    next_window_place({work.pos.x, work.pos.y + work.size.y - theme::STATUS_BAR}, {work.size.x, theme::STATUS_BAR});
    Style style = kit().style;
    style.item_spacing = {0.0f, 0.0f};
    push_style(style);
    open_ = begin_window("##StatusBar",
                         {.title_bar = false,
                          .resize = false,
                          .move = false,
                          .collapse = false,
                          .scrollbar = false,
                          .background = false,
                          .docking = false,
                          .nav_focus = false,
                          .remembered = false},
                         {theme::GAP_S, 0.0f});
    if (open_) {
        const core::Rect rect = ui().area_rect();
        core::DrawList& dl = ui().draw_list();
        dl.rect_filled(rect.min, rect.max, faded(theme::surface_sunken()));
        dl.rect_filled(rect.min, {rect.max.x, rect.min.y + 1.0f}, faded(theme::border()));
    }
}

StatusBar::~StatusBar() {
    if (open_ && right_start_ >= 0.0f) {
        // How wide the right-hand group came out, so the next frame's
        // starts where it ends against the edge.
        same_line();
        store_float(ui().id(RIGHT_WIDTH), cursor().x - right_start_);
    }
    end_window();
    pop_style();
}

void StatusBar::right() {
    if (!open_) return;
    const float width = stored_float(ui().id(RIGHT_WIDTH), 0.0f);
    const float edge = ui().area_inside().max.x;
    // A spacer up to where the group starts.
    same_line();
    const float x = cursor().x;
    const float gap = std::max(edge - width - x, 0.0f);
    reserve({gap, theme::STATUS_BAR});
    right_start_ = x + gap;
}

void status_item(const char* icon, std::string_view text, bool unsaved) {
    same_line();
    const float pad = theme::GAP_M;
    const glm::vec2 start = cursor();
    const float text_y = line_y(line_height());
    float x = start.x + pad;

    if (icon != nullptr) {
        draw_text({x, text_y}, theme::text_disabled(), icon);
        x += measure(icon).x + theme::GAP_S + 1.0f;
    }
    draw_text({x, text_y}, theme::text_secondary(), text);
    x += measure(text).x;
    if (unsaved) {
        x += theme::GAP_S + UNSAVED_DOT_RADIUS;
        unsaved_dot(DrawList::window(), {x, start.y + theme::STATUS_BAR * 0.5f});
        x += UNSAVED_DOT_RADIUS;
    }
    reserve({x + pad - start.x, theme::STATUS_BAR});
    if (unsaved && item_hovered()) tooltip("Unsaved changes");
}

void status_figure(std::string_view text, std::string_view widest) {
    same_line();
    const float pad = theme::GAP_M;
    const glm::vec2 start = cursor();
    const float text_w = measure(text).x;
    const float width = std::max(measure(widest).x, text_w);
    draw_text({start.x + pad + width - text_w, line_y(line_height())}, theme::text_secondary(), text);
    reserve({pad + width + pad, theme::STATUS_BAR});
}

void status_busy(std::string_view text) {
    same_line();
    const float pad = theme::GAP_M;
    const glm::vec2 start = cursor();
    // The spinner takes an icon's room.
    const float radius = std::floor(line_height() * 0.35f);
    float x = start.x + pad;
    spinner(DrawList::window(), {x + radius, start.y + theme::STATUS_BAR * 0.5f}, radius,
            static_cast<float>(seconds()));
    x += radius * 2.0f + theme::GAP_S + 1.0f;
    draw_text({x, line_y(line_height())}, theme::text_secondary(), text);
    x += measure(text).x;
    reserve({x + pad - start.x, theme::STATUS_BAR});
}

void status_note(std::string_view text, std::string_view where) {
    same_line();
    const float pad = theme::GAP_M;
    const glm::vec2 start = cursor();
    float x = start.x + pad;
    draw_text({x, line_y(line_height())}, theme::text_secondary(), text);
    x += measure(text).x;
    if (!where.empty()) {
        const FontScope mono(theme::mono_font(), theme::SMALL_TEXT);
        x += theme::GAP_S;
        draw_text({x, line_y(line_height())}, theme::text_disabled(), where);
        x += measure(where).x;
    }
    reserve({x + pad - start.x, theme::STATUS_BAR});
}

void status_rule() {
    same_line();
    const glm::vec2 start = cursor();
    const float h = 12.0f;
    const float y = start.y + (theme::STATUS_BAR - h) * 0.5f;
    ui().draw_list().rect_filled({start.x, y}, {start.x + 1.0f, y + h}, faded(theme::surface_highest()));
    reserve({1.0f, theme::STATUS_BAR});
}

void status_pill(const char* icon, const char* text, StatusTone tone) {
    same_line();
    const Color colour = tone_colour(tone);
    const float pad = theme::GAP_M;
    const float inner = 7.0f;
    const float height = 18.0f;
    const glm::vec2 start = cursor();
    const float top = start.y + (theme::STATUS_BAR - height) * 0.5f;
    // An icon and the gap after it, when there is one.
    const float icon_w = icon != nullptr ? measure(icon).x + theme::GAP_S : 0.0f;
    const float text_w = measure(text).x;
    const float width = inner + icon_w + text_w + inner;
    const glm::vec2 min{start.x + pad, top};
    const glm::vec2 max{min.x + width, top + height};

    if (tone != StatusTone::Plain) {
        ui().draw_list().rect_filled(min, max, faded(with_alpha(colour, 0.16f)), height * 0.5f);
    }
    const float text_y = line_y(line_height());
    if (icon != nullptr) draw_text({min.x + inner, text_y}, colour, icon);
    draw_text({min.x + inner + icon_w, text_y}, colour, text);
    reserve({pad + width + pad, theme::STATUS_BAR});
}

void status_state(StatusTone tone, std::string_view name, std::string_view tip) {
    same_line();
    const float pad = theme::GAP_M;
    const glm::vec2 start = cursor();
    float x = start.x + pad;
    ui().draw_list().circle_filled({x + STATE_DOT * 0.5f, start.y + theme::STATUS_BAR * 0.5f}, STATE_DOT * 0.5f,
                                   faded(tone_colour(tone)));
    x += STATE_DOT + theme::GAP_S;
    draw_text({x, line_y(line_height())}, theme::text_secondary(), name);
    x += measure(name).x;
    reserve({x + pad - start.x, theme::STATUS_BAR});
    if (!tip.empty() && item_hovered()) tooltip(tip);
}

bool status_problems(int errors, int warnings, std::string_view newest, std::string_view where) {
    if (errors <= 0 && warnings <= 0) return false;
    core::Context& context = ui();
    same_line();
    const float inner = theme::GAP_M;
    const float height = 20.0f;
    const glm::vec2 start = cursor();
    const float top = start.y + (theme::STATUS_BAR - height) * 0.5f;

    const std::string error_count = std::to_string(errors);
    const std::string warning_count = std::to_string(warnings);
    const float error_w = measure(icon::error).x + COUNT_GAP + measure(error_count).x;
    const float warning_w = measure(icon::warning).x + COUNT_GAP + measure(warning_count).x;
    const std::string sentence = ellipsized(newest, NEWEST_WIDTH);
    const float sentence_w = sentence.empty() ? 0.0f : theme::GAP_M + measure(sentence).x;
    float where_w = 0.0f;
    if (!sentence.empty() && !where.empty()) {
        const FontScope mono(theme::mono_font(), theme::SMALL_TEXT);
        where_w = theme::GAP_S + measure(where).x;
    }
    const float width = inner + error_w + theme::GAP_M + warning_w + sentence_w + where_w + inner;

    // As the Content button: the bar's whole height is the item, its wash
    // the shorter pill inside.
    const core::Rect rect = context.layout().next({width + theme::GAP_S, theme::STATUS_BAR});
    const Id id = context.id("##status_problems");
    const core::Interaction in = place(id, rect);
    context.semantics(id, core::Role::Button, "Problems");
    const glm::vec2 min{start.x, top};
    const glm::vec2 max{start.x + width, top + height};
    if (in.hovered) ui().draw_list().rect_filled(min, max, faded(theme::surface_hover()), 4.0f);

    const float text_y = line_y(line_height());
    float x = min.x + inner;
    draw_text({x, text_y}, theme::error(), icon::error);
    x += measure(icon::error).x + COUNT_GAP;
    draw_text({x, text_y}, theme::error(), error_count);
    x += measure(error_count).x + theme::GAP_M;
    draw_text({x, text_y}, theme::warning(), icon::warning);
    x += measure(icon::warning).x + COUNT_GAP;
    draw_text({x, text_y}, theme::warning(), warning_count);
    x += measure(warning_count).x;
    if (!sentence.empty()) {
        x += theme::GAP_M;
        draw_text({x, text_y}, in.hovered ? theme::text() : theme::text_secondary(), sentence);
        x += measure(sentence).x;
        if (!where.empty()) {
            const FontScope mono(theme::mono_font(), theme::SMALL_TEXT);
            x += theme::GAP_S;
            draw_text({x, line_y(line_height())}, theme::text_disabled(), where);
        }
    }
    if (in.hovered && sentence != newest) item_tooltip(newest);
    return in.clicked;
}

bool status_button(const char* icon, const char* label, const char* shortcut, bool open) {
    core::Context& context = ui();
    same_line();
    const float inner = theme::GAP_M;
    const float height = 20.0f;
    const glm::vec2 start = cursor();
    const float top = start.y + (theme::STATUS_BAR - height) * 0.5f;

    const float icon_w = measure(icon).x;
    const float label_w = measure(label).x;
    glm::vec2 key{0.0f};
    {
        FontScope mono(Font::Mono, theme::SMALL_TEXT);
        key = measure(shortcut);
    }
    const float width = inner + icon_w + theme::GAP_S + 2.0f + label_w + theme::GAP_M + key.x + inner;

    // The button takes the bar's whole height, so it is the item that
    // follows on the line; its wash is the shorter pill inside.
    const core::Rect rect = context.layout().next({width + theme::GAP_S, theme::STATUS_BAR});
    Id id = 0;
    {
        IdScope scope(label);
        id = context.id("##status_button");
    }
    const core::Interaction in = place(id, rect);
    context.semantics(id, core::Role::Button, label, {}, {.selected = open});
    const glm::vec2 min{start.x, top};
    const glm::vec2 max{start.x + width, top + height};

    core::DrawList& dl = context.draw_list();
    if (open) {
        dl.rect_filled(min, max, faded(theme::selection()), 4.0f);
    } else if (in.hovered) {
        dl.rect_filled(min, max, faded(theme::surface_hover()), 4.0f);
    }
    const float text_y = line_y(line_height());
    float x = min.x + inner;
    draw_text({x, text_y}, open ? theme::accent() : theme::text_secondary(), icon);
    x += icon_w + theme::GAP_S + 2.0f;
    draw_text({x, text_y}, open || in.hovered ? theme::text() : theme::text_secondary(), label);
    x += label_w + theme::GAP_M;
    FontScope mono(Font::Mono, theme::SMALL_TEXT);
    draw_text({x, line_y(key.y)}, theme::text_disabled(), shortcut);
    return in.clicked;
}

} // namespace ftk::ui
