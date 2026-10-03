#include "ftk/ui/kit/status_bar.hpp"

#include "ftk/ui/draw_list.hpp"
#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/feedback.hpp"
#include "ftk/ui/kit/item.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/state.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/tooltip.hpp"
#include "ftk/ui/kit/window.hpp"

#include <algorithm>

namespace ftk::ui {

using namespace corekit;

namespace {

// Where text `height` tall sits on the bar's line, centred in its height.
float line_y(float height) {
    return window_pos().y + (theme::STATUS_BAR - height) * 0.5f;
}

Color with_alpha(Color colour, float alpha) {
    colour.a = alpha;
    return colour;
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
    const Color colour = tone == StatusTone::Success ? theme::success()
                       : tone == StatusTone::Warning ? theme::warning()
                                                     : theme::text_secondary();
    const float pad = theme::GAP_M;
    const float inner = 7.0f;
    const float height = 18.0f;
    const glm::vec2 start = cursor();
    const float top = start.y + (theme::STATUS_BAR - height) * 0.5f;
    const float icon_w = measure(icon).x;
    const float text_w = measure(text).x;
    const float width = inner + icon_w + theme::GAP_S + text_w + inner;
    const glm::vec2 min{start.x + pad, top};
    const glm::vec2 max{min.x + width, top + height};

    if (tone != StatusTone::Plain) {
        ui().draw_list().rect_filled(min, max, faded(with_alpha(colour, 0.16f)), height * 0.5f);
    }
    const float text_y = line_y(line_height());
    draw_text({min.x + inner, text_y}, colour, icon);
    draw_text({min.x + inner + icon_w + theme::GAP_S, text_y}, colour, text);
    reserve({pad + width + pad, theme::STATUS_BAR});
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
