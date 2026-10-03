#include "ftk/ui/kit/inset_group.hpp"

#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/item.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/state.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/tooltip.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace ftk::ui {

using namespace corekit;

namespace {

// The box's inner padding: a little more under the last row.
constexpr float PAD = theme::GAP_S + theme::GAP_XS;
constexpr float PAD_BOTTOM = theme::GAP_M;
constexpr float HEADING_SIZE = 11.5f;

} // namespace

InsetGroup::InsetGroup(const char* id, const char* heading, const char* icon, const Color& icon_colour)
    : InsetGroup(id, InsetHeading{.text = heading, .icon = icon, .icon_colour = icon_colour}) {}

InsetGroup::InsetGroup(const char* id, const InsetHeading& heading) {
    core::Context& context = ui();
    // Its ids under its own until it ends.
    context.ids().push(id);
    const Id open_id = context.id("##open");
    height_id_ = context.id("##height");
    open_ = stored_bool(open_id, true);

    min_ = cursor();
    width_ = space_left().x;
    // The backdrop goes behind the contents, whose height is only known once
    // they are drawn: it is drawn at the height the box had last frame.
    const float heading_h = frame_height();
    const float height = open_ ? stored_float(height_id_, heading_h + PAD * 2.0f) : heading_h + PAD;
    core::DrawList& dl = context.draw_list();
    dl.rect_filled(min_, {min_.x + width_, min_.y + height}, faded(theme::surface_sunken()),
                   kit().style.frame_rounding);

    // The heading: the whole row but the icons at its right folds the box.
    const float remove_w = heading.remove != nullptr ? heading_h : 0.0f;
    const float action_w = heading.action != nullptr ? heading_h : 0.0f;
    const float live_w = heading.live != nullptr ? theme::GAP_M + theme::GAP_S : 0.0f;
    const float fold_w = std::max(width_ - remove_w - action_w - live_w - PAD, 1.0f);
    const float row_y = min_.y + PAD * 0.5f;
    set_cursor({min_.x, row_y});
    const core::Rect fold = context.layout().next({fold_w, heading_h});
    if (place(context.id("##fold"), fold).clicked) {
        open_ = !open_;
        store_bool(open_id, open_);
    }

    const bool bright = heading.named || heading.code;
    const Color secondary = theme::text_secondary();
    float x = min_.x + PAD;
    float y = 0.0f;
    {
        FontScope bold(Font::Bold, bright ? theme::BODY_TEXT : HEADING_SIZE);
        y = row_y + (heading_h - font_size()) * 0.5f;
        const char* chevron = open_ ? icon::fold_open : icon::fold_closed;
        draw_text({x, y}, secondary, chevron);
        x += measure(chevron).x + theme::GAP_S;
        if (heading.icon != nullptr) {
            draw_text({x, y}, heading.icon_colour, heading.icon);
            x += measure(heading.icon).x + theme::GAP_S + theme::GAP_XS;
        }
    }
    std::string text(heading.text);
    if (!bright) {
        for (char& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    float text_right = min_.x + fold_w;
    // Folded, what the box holds reads at the heading's right.
    if (!open_ && heading.folded_summary != nullptr) {
        const float summary_x = min_.x + fold_w - measure(heading.folded_summary).x - theme::GAP_S;
        draw_text({summary_x, row_y + (heading_h - font_size()) * 0.5f}, secondary, heading.folded_summary);
        text_right = summary_x - theme::GAP_M;
    }
    dl.push_clip(min_, {text_right, min_.y + heading_h + PAD});
    {
        FontScope face(heading.code ? Font::Mono : Font::Bold, bright ? theme::BODY_TEXT : HEADING_SIZE);
        draw_text({x, y}, bright ? theme::text() : secondary, text);
        x += measure(text).x + theme::GAP_S;
    }
    if (heading.detail != nullptr) {
        draw_text({x, row_y + (heading_h - font_size()) * 0.5f}, secondary, heading.detail);
    }
    dl.pop_clip();

    // The icons at the right, right to left: remove, the action, the dot.
    float icons_x = min_.x + width_ - PAD * 0.5f - remove_w;
    if (heading.action != nullptr) {
        icons_x -= action_w;
        set_cursor({icons_x, row_y});
        if (icon_button("##action", heading.action_icon, heading.action_tooltip)) *heading.action = true;
    }
    if (heading.live != nullptr) {
        icons_x -= live_w;
        set_cursor({icons_x, row_y});
        reserve({live_w, heading_h});
        dl.circle_filled({icons_x + live_w * 0.5f, row_y + heading_h * 0.5f}, 3.5f, faded(theme::success()));
        item_tooltip(heading.live);
    }
    if (heading.remove != nullptr) {
        set_cursor({min_.x + width_ - PAD * 0.5f - remove_w, row_y});
        if (icon_button("##remove", icon::remove, heading.remove_tooltip, ButtonKind::GhostDanger)) {
            *heading.remove = true;
        }
    }

    if (!open_) {
        set_cursor({min_.x, min_.y + height});
        reserve({width_, 0.0f});
        context.ids().pop();
        return;
    }

    // The contents, inside the padding, on the darker field colour a sunken
    // box needs; anything sized to the room left ends at the padding.
    core::Layout& layout = context.layout();
    const glm::vec2 start{min_.x + PAD, row_y + heading_h + theme::GAP_XS};
    layout.begin_area({.min = start, .max = {min_.x + width_ - PAD, std::max(layout.area().max.y, start.y)}});
    kit().frames.push_back(
        {.rest = theme::surface_inset(), .hover = theme::surface_base(), .active = theme::surface_raised()});
}

InsetGroup::~InsetGroup() {
    core::Context& context = ui();
    if (open_) {
        kit().frames.pop_back();
        const float top = min_.y + PAD * 0.5f + frame_height() + theme::GAP_XS;
        const glm::vec2 held = context.layout().end_area();
        const float height = top + held.y - min_.y + PAD_BOTTOM;
        store_float(height_id_, height);
        set_cursor({min_.x, min_.y + height});
        reserve({width_, 0.0f});
        context.ids().pop();
    }
}

} // namespace ftk::ui
