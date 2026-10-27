#include "ftk/ui/kit/plate.hpp"

#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/state.hpp"

#include <algorithm>
#include <cmath>

namespace ftk::ui {

using namespace corekit;

namespace {
constexpr float PAD = 6.0f;       // inside the plate
constexpr float ITEM_GAP = 4.0f;  // between its items along a row
constexpr float ROW_GAP = 6.0f;   // between its rows
constexpr float ROUNDING = 6.0f;
// Its shadow falls this far below it, fading out over the softness.
constexpr float SHADOW_DROP = 6.0f;
constexpr float SHADOW_SOFTNESS = 18.0f;
} // namespace

Plate::Plate(const char* id, glm::vec2 view_min, glm::vec2 view_max, PlatePlace where, float width,
             bool over_windows)
    : width_(width) {
    core::Context& context = ui();
    context.ids().push(id);
    height_id_ = context.id("##height");
    cursor_ = cursor();
    // As tall as it was last frame, which is how tall it is but on the
    // frame it opens.
    const float height = stored_float(height_id_, 0.0f);
    const float x = where == PlatePlace::TopCenter ? (view_min.x + view_max.x - width) * 0.5f
                                                   : view_max.x - PLATE_INSET - width;
    const float y = where == PlatePlace::BottomRight ? view_max.y - PLATE_INSET - height : view_min.y + PLATE_INSET;
    min_ = {std::round(x), std::round(y)};

    // It takes the pointer over all of it, beneath what it holds: placed
    // first, letting what follows go over it. A press on it, or on a button
    // it holds, leaves the keyboard where it is.
    const core::Rect area{.min = min_, .max = min_ + glm::vec2{width, height}};
    // Over the windows, a panel just its size, brought in front of them each
    // frame. A panel's shapes are clipped as the OS window's, so its shadow
    // still falls past it.
    if (over_windows) {
        panel_ = context.id("##panel");
        context.begin_panel(panel_, area, {.padding = glm::vec2{0.0f}, .scroll = false});
        context.bring_to_front(panel_);
    }
    ++kit().keeps_keyboard;
    kit().next.allow_overlap = true;
    (void)place(context.id("##plate"), area, {.left = true, .right = true, .middle = true});

    mark_ = context.draw_list().mark();
    Style style = kit().style;
    style.item_spacing = {ITEM_GAP, ROW_GAP};
    push_style(style);
    // What it holds is laid out inside it, in from its edges by its
    // padding and down to the view's foot, so an item that takes the room
    // left (a rule, a row) ends at its padding. It is no item of the
    // layout it floats over.
    context.layout().begin_area({.min = min_ + glm::vec2{PAD}, .max = {min_.x + width - PAD, view_max.y}});
}

Plate::~Plate() {
    core::Context& context = ui();
    const glm::vec2 held = context.layout().end_area();
    pop_style();
    --kit().keeps_keyboard;

    // Its surface, drawn once what it holds is and put beneath that.
    const glm::vec2 max{min_.x + width_, min_.y + PAD + held.y + PAD};
    core::DrawList& dl = context.draw_list();
    const std::size_t split = dl.mark();
    const glm::vec2 drop{0.0f, SHADOW_DROP};
    dl.shadow(min_ + drop, max + drop, faded(theme::shadow()), ROUNDING, SHADOW_SOFTNESS);
    dl.rect_filled(min_, max, faded(theme::surface_sunken()), ROUNDING);
    dl.rect(min_, max, faded(theme::surface_raised()), ROUNDING);
    dl.put_under(mark_, split);

    store_float(height_id_, max.y - min_.y);
    if (panel_ != 0) context.end_panel();
    set_cursor(cursor_);
    context.ids().pop();
}

float Plate::inner_width() const {
    return width_ - 2.0f * PAD;
}

namespace {
// A card's look, a popup's: rounder corners than a frame's are for a
// plate, a deeper shadow. It hangs this far from its anchor, and stays
// this far in from the view's edges.
constexpr float CARD_ROUNDING = theme::FRAME_ROUNDING;
constexpr float CARD_SHADOW_DROP = 8.0f;
constexpr float CARD_SHADOW_SOFTNESS = 24.0f;
constexpr float CARD_GAP = 2.0f;
} // namespace

Card::Card(const char* id, const CardPlace& place) : width_(place.width), padding_(place.padding) {
    core::Context& context = ui();
    context.ids().push(id);
    height_id_ = context.id("##height");
    cursor_ = cursor();

    // Under its anchor, or over it when it doesn't fit under and there is
    // more room above; as tall as what it held last frame, but no taller
    // than the room on that side. Kept inside the view, in from its edges.
    const float held = stored_float(height_id_, 0.0f);
    const float room_below = place.view_max.y - PLATE_INSET - place.anchor_max.y - CARD_GAP;
    const float room_above = place.anchor_min.y - CARD_GAP - place.view_min.y - PLATE_INSET;
    const bool above = held > room_below && room_above > room_below;
    const float shown = std::round(std::min(held, std::max(above ? room_above : room_below, 0.0f)));
    const float top = above ? place.anchor_min.y - CARD_GAP - shown : place.anchor_max.y + CARD_GAP;
    const float left = std::clamp(place.anchor_min.x, place.view_min.x + PLATE_INSET,
                                  std::max(place.view_min.x + PLATE_INSET, place.view_max.x - PLATE_INSET - width_));
    min_ = {std::round(left), std::round(top)};
    // Under its anchor it may grow down to the room's end this frame; over
    // it, its foot stays at the anchor, and it grows from the next.
    room_ = above ? shown : std::round(std::max(room_below, 0.0f));

    ++kit().keeps_keyboard;
    mark_ = context.draw_list().mark();
    Style style = kit().style;
    style.item_spacing = {ITEM_GAP, ROW_GAP};
    push_style(style);

    // What it holds is laid out in a region of its own, the room it has
    // tall: clipped at its edge, and scrolled by the wheel over it. A new
    // region each time it opens, so what it holds starts at its top.
    const auto frame = static_cast<int>(context.frame() & 0x3fffffffU);
    const Id drawn_id = context.id("##drawn");
    const Id opened_id = context.id("##opened");
    const bool shown_before = stored_int(drawn_id, -1) == ((frame - 1) & 0x3fffffff);
    store_int(drawn_id, frame);
    const int opened = stored_int(opened_id, 0) + (shown_before ? 0 : 1);
    store_int(opened_id, opened);
    context.layout().begin_area({.min = min_, .max = min_ + glm::vec2{width_, room_}});
    context.ids().push_id(static_cast<Id>(opened));
    context.begin_region("##held", {width_, room_}, {.wheel_scrolls = true, .padding = padding_});
    // It takes the pointer over all of it, as tall as it was, beneath what
    // it holds. Placed in its region, the wheel over it turns what it holds,
    // never the view under it.
    const core::Rect area{.min = min_, .max = min_ + glm::vec2{width_, shown}};
    kit().next.allow_overlap = true;
    (void)corekit::place(context.id("##card"), area, {.left = true, .right = true, .middle = true});
    context.layout().begin_group();
    inner_min_ = context.layout().cursor();
}

Card::~Card() {
    core::Context& context = ui();
    const float held = context.layout().end_group().height();
    const glm::vec2 max = this->max(held);

    // While it holds more than it shows, the window's scrollbar down its
    // right edge, inside its border and clear of its corners.
    const core::Rect track{.min = {max.x - 1.0f - kit().style.scrollbar_size, min_.y + CARD_ROUNDING},
                           .max = {max.x - 1.0f, max.y - CARD_ROUNDING}};
    float offset = context.scroll().y;
    if (scrollbar_at(context.id("##scroll"), track, offset, track.height(), context.scroll_max().y)) {
        context.set_scroll({context.scroll().x, offset});
    }
    context.end_region();
    context.ids().pop();
    (void)context.layout().end_area();
    pop_style();
    --kit().keeps_keyboard;

    // Its surface as it shows, drawn once what it holds is and put beneath.
    core::DrawList& dl = context.draw_list();
    const std::size_t split = dl.mark();
    const glm::vec2 drop{0.0f, CARD_SHADOW_DROP};
    dl.shadow(min_ + drop, max + drop, faded(theme::shadow()), CARD_ROUNDING, CARD_SHADOW_SOFTNESS);
    dl.rect_filled(min_, max, faded(theme::surface_sunken()), CARD_ROUNDING);
    dl.rect(min_, max, faded(theme::surface_raised()), CARD_ROUNDING);
    dl.put_under(mark_, split);

    // How tall it holds is where it goes next frame: a new height is drawn
    // there at once.
    const float full = padding_.y + held + padding_.y;
    if (full != stored_float(height_id_, 0.0f)) context.request_frame();
    store_float(height_id_, full);
    set_cursor(cursor_);
    context.ids().pop();
}

glm::vec2 Card::max(float held) const {
    return {min_.x + width_, min_.y + std::min(padding_.y + held + padding_.y, room_)};
}

float Card::inner_width() const {
    return width_ - 2.0f * padding_.x;
}

} // namespace ftk::ui
