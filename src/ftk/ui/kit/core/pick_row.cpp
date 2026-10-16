#include "ftk/ui/kit/pick_row.hpp"

#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/loading.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/input.hpp"
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

// Room at the row's ends, the box, the icon's column, the gap between the
// row's parts, the corner, and the bar along the foot of a row at work.
constexpr float ROW_PAD = 6.0f;
constexpr float BOX = 16.0f;
constexpr float ICON_COLUMN = 18.0f;
constexpr float PART_GAP = 6.0f;
constexpr float ROUNDING = 3.0f;
constexpr float BAR = 2.0f;
constexpr float SPINNER_RADIUS = 5.0f;

} // namespace

PickRowResult pick_row(const PickRowSpec& spec, bool selected) {
    PickRowResult result;
    core::Context& context = ui();
    IdScope scope(spec.id);
    const Id row_id = context.id("##row");
    const Id box_id = context.id("##pick");

    const float height = theme::TREE_ROW;
    const glm::vec2 min = cursor();
    const float width = std::max(space_left().x, 1.0f);
    const glm::vec2 max{min.x + width, min.y + height};
    if (can_skip_row(row_id, {.min = min, .max = max})) {
        place_still(context.layout().next({width, height}));
        return result;
    }

    const core::Rect row = context.layout().next({width, height});
    const core::Interaction in = place(row_id, row);
    const core::Rect box{.min = {min.x + ROW_PAD, std::floor(min.y + ((height - BOX) * 0.5f))},
                         .max = {min.x + ROW_PAD + BOX, std::floor(min.y + ((height - BOX) * 0.5f)) + BOX}};
    // The box takes the press around it, half a gap either side.
    const float pointer = in.press_position.x;
    const bool on_box = pointer >= box.min.x - (PART_GAP * 0.5f) && pointer < box.max.x + (PART_GAP * 0.5f);
    result.toggled = in.clicked && on_box && !spec.locked;
    result.clicked = in.clicked && !on_box;
    result.double_clicked = in.hovered && in.double_clicked && !on_box;
    context.semantics(row_id, core::Role::ListItem, spec.name, spec.figure, {.selected = selected});
    context.semantics(box_id, core::Role::Toggle, spec.name, {}, {.disabled = spec.locked, .checked = spec.picked}, box);

    core::DrawList& dl = context.draw_list();
    if (selected) {
        dl.rect_filled(min, max, faded(theme::selection()), ROUNDING);
    } else if (in.hovered) {
        dl.rect_filled(min, max, faded(theme::surface_hover()), ROUNDING);
    }

    {
        const Disabled locked(spec.locked);
        draw_check_box(box, in, spec.picked, false);
    }

    const float body = font_size();
    const float text_y = std::floor(min.y + ((height - body) * 0.5f));
    const bool waiting = spec.work == PickRowWork::waiting;
    float x = box.max.x + PART_GAP;
    if (spec.icon != nullptr) {
        Color colour = spec.hue ? theme::hue(*spec.hue) : theme::text_secondary();
        if (waiting) colour = theme::text_disabled();
        draw_text({x + ((ICON_COLUMN - measure(spec.icon).x) * 0.5f), text_y}, colour, spec.icon);
        x += ICON_COLUMN + PART_GAP;
    }

    // What stands at the right, level with the name.
    float name_right = max.x - ROW_PAD;
    switch (spec.work) {
        case PickRowWork::running: {
            spinner(DrawList::window(), {name_right - SPINNER_RADIUS, std::floor(min.y + (height * 0.5f))},
                    SPINNER_RADIUS, static_cast<float>(seconds()));
            name_right -= (SPINNER_RADIUS * 2.0f) + PART_GAP;
            break;
        }
        case PickRowWork::done:
        case PickRowWork::failed: {
            const bool done = spec.work == PickRowWork::done;
            const char* glyph = done ? icon::success : icon::error;
            const float glyph_w = measure(glyph).x;
            draw_text({name_right - glyph_w, text_y}, done ? theme::success() : theme::error(), glyph);
            name_right -= glyph_w + PART_GAP;
            break;
        }
        case PickRowWork::none:
        case PickRowWork::waiting: {
            if (spec.figure.empty()) break;
            const FontScope mono(theme::mono_font(), theme::SMALL_TEXT);
            const float figure_w = measure(spec.figure).x;
            draw_text({name_right - figure_w, std::floor(text_y + ((body - font_size()) * 0.5f))},
                      theme::text_disabled(), spec.figure);
            name_right -= figure_w + (PART_GAP * 2.0f);
            break;
        }
    }

    const float room = std::max(name_right - x, 1.0f);
    const bool cut = measure(spec.name).x > room;
    const Color name_colour = waiting ? theme::text_disabled() : theme::text();
    if (cut) {
        draw_text({x, text_y}, name_colour, ellipsized(spec.name, room));
    } else {
        detail::draw_highlighted(DrawList::window(), {x, text_y}, spec.name, spec.query, name_colour);
    }

    if (spec.work == PickRowWork::running) {
        const float bar_left = box.max.x + PART_GAP;
        const float bar_right = max.x - ROW_PAD;
        const float share = std::clamp(spec.progress, 0.0f, 1.0f);
        dl.rect_filled({bar_left, max.y - BAR}, {bar_right, max.y}, faded(theme::surface_highest()), 1.0f);
        if (share > 0.0f) {
            dl.rect_filled({bar_left, max.y - BAR}, {bar_left + ((bar_right - bar_left) * share), max.y},
                           faded(theme::accent()), 1.0f);
        }
    }

    if (in.hovered && !context.dragging()) {
        std::string tip;
        if (cut) tip = std::string(spec.name);
        if (!spec.tooltip.empty()) {
            if (!tip.empty()) tip += '\n';
            tip += spec.tooltip;
        }
        if (!tip.empty()) item_tooltip(tip);
    }
    return result;
}

} // namespace ftk::ui
