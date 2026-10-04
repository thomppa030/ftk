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

Plate::Plate(const char* id, glm::vec2 view_min, glm::vec2 view_max, PlatePlace where, float width) : width_(width) {
    core::Context& context = ui();
    context.ids().push(id);
    height_id_ = context.id("##height");
    cursor_ = cursor();
    const float x = where == PlatePlace::TopRight ? view_max.x - PLATE_INSET - width
                                                  : (view_min.x + view_max.x - width) * 0.5f;
    min_ = {std::round(x), view_min.y + PLATE_INSET};

    // It takes the pointer over all of it, beneath what it holds: placed
    // first, letting what follows go over it. As tall as it was last frame,
    // which is how tall it is but on the frame it opens. A press on it, or
    // on a button it holds, leaves the keyboard where it is.
    const core::Rect area{.min = min_, .max = min_ + glm::vec2{width, stored_float(height_id_, 0.0f)}};
    ++kit().keeps_keyboard;
    kit().next.allow_overlap = true;
    (void)place(context.id("##plate"), area, {.left = true, .right = true, .middle = true});

    mark_ = context.draw_list().mark();
    Style style = kit().style;
    style.item_spacing = {ITEM_GAP, ROW_GAP};
    push_style(style);
    set_cursor(min_ + glm::vec2{PAD});
    context.layout().begin_group();
}

Plate::~Plate() {
    core::Context& context = ui();
    const core::Rect held = context.layout().end_group();
    pop_style();
    --kit().keeps_keyboard;

    // Its surface, drawn once what it holds is and put beneath that.
    const glm::vec2 max{min_.x + width_, held.max.y + PAD};
    core::DrawList& dl = context.draw_list();
    const std::size_t split = dl.mark();
    const glm::vec2 drop{0.0f, SHADOW_DROP};
    dl.shadow(min_ + drop, max + drop, faded(theme::shadow()), ROUNDING, SHADOW_SOFTNESS);
    dl.rect_filled(min_, max, faded(theme::surface_sunken()), ROUNDING);
    dl.rect(min_, max, faded(theme::surface_raised()), ROUNDING);
    dl.put_under(mark_, split);

    store_float(height_id_, max.y - min_.y);
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
    // more room above; as tall as it was last frame. Kept inside the view.
    const float height = stored_float(height_id_, 0.0f);
    const float room_below = place.view_max.y - place.anchor_max.y - CARD_GAP;
    const float room_above = place.anchor_min.y - place.view_min.y - CARD_GAP;
    const bool above = height > room_below && room_above > room_below;
    const float top = above ? place.anchor_min.y - CARD_GAP - height : place.anchor_max.y + CARD_GAP;
    const float left = std::clamp(place.anchor_min.x, place.view_min.x + PLATE_INSET,
                                  std::max(place.view_min.x + PLATE_INSET, place.view_max.x - PLATE_INSET - width_));
    min_ = {std::round(left), std::round(top)};

    const core::Rect area{.min = min_, .max = min_ + glm::vec2{width_, height}};
    ++kit().keeps_keyboard;
    kit().next.allow_overlap = true;
    (void)corekit::place(context.id("##card"), area, {.left = true, .right = true, .middle = true});

    mark_ = context.draw_list().mark();
    Style style = kit().style;
    style.item_spacing = {ITEM_GAP, ROW_GAP};
    push_style(style);
    set_cursor(min_ + padding_);
    context.layout().begin_group();
}

Card::~Card() {
    core::Context& context = ui();
    const core::Rect held = context.layout().end_group();
    pop_style();
    --kit().keeps_keyboard;

    const glm::vec2 max{min_.x + width_, std::max(held.max.y, min_.y + padding_.y) + padding_.y};
    core::DrawList& dl = context.draw_list();
    const std::size_t split = dl.mark();
    const glm::vec2 drop{0.0f, CARD_SHADOW_DROP};
    dl.shadow(min_ + drop, max + drop, faded(theme::shadow()), CARD_ROUNDING, CARD_SHADOW_SOFTNESS);
    dl.rect_filled(min_, max, faded(theme::surface_sunken()), CARD_ROUNDING);
    dl.rect(min_, max, faded(theme::surface_raised()), CARD_ROUNDING);
    dl.put_under(mark_, split);

    store_float(height_id_, max.y - min_.y);
    set_cursor(cursor_);
    context.ids().pop();
}

float Card::inner_width() const {
    return width_ - 2.0f * padding_.x;
}

} // namespace ftk::ui
