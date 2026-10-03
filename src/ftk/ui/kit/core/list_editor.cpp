#include "ftk/ui/kit/list_editor.hpp"

#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/edit_record.hpp"
#include "ftk/ui/kit/field.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/input.hpp"
#include "ftk/ui/kit/item.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/state.hpp"
#include "ftk/ui/kit/text.hpp"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

namespace ftk::ui {

using namespace corekit;

namespace {

// A card's insides: padding (left wider, where the grip is), the grip's and
// the number's widths.
constexpr float PAD_LEFT = 6.0f;
constexpr float PAD = 4.0f;
constexpr float GRIP_W = 16.0f;
constexpr float INDEX_W = 22.0f;
constexpr float INSERT_LINE = 2.0f;
constexpr float DASH = 4.0f;

// What a grip carries while an item is dragged: which list, which item.
struct Dragged {
    Id list;
    std::size_t index;
};
constexpr const char* PAYLOAD = "FJ_LIST_ITEM";

void dashed_rect(core::Rect rect, Color colour) {
    core::DrawList& dl = ui().draw_list();
    const auto dashes = [&](glm::vec2 a, glm::vec2 b) {
        const float length = glm::distance(a, b);
        const glm::vec2 dir = (b - a) / length;
        for (float t = 0.0f; t < length; t += DASH * 2.0f) {
            dl.line(a + dir * t, a + dir * std::min(t + DASH, length), faded(colour), 1.0f);
        }
    };
    dashes(rect.min, {rect.max.x, rect.min.y});
    dashes({rect.max.x, rect.min.y}, rect.max);
    dashes(rect.max, {rect.min.x, rect.max.y});
    dashes({rect.min.x, rect.max.y}, rect.min);
}

} // namespace

ListEditor::ListEditor(const char* id, std::size_t count, ListOptions options) : options_{options} {
    core::Context& context = ui();
    context.ids().push(id);
    list_id_ = context.id("##list");
    // A long list scrolls in a box of its own, so the rest of the panel
    // stays within reach.
    if (options_.max_cards > 0 && count > static_cast<std::size_t>(options_.max_cards)) {
        const float card = frame_height() + PAD * 2.0f + theme::GAP_S;
        context.begin_region("##cards", {core::FILL, card * static_cast<float>(options_.max_cards)});
        scrolling_ = true;
    }
}

ListEditor::~ListEditor() {
    end_scroll();
    ui().ids().pop();
}

void ListEditor::end_scroll() {
    if (!scrolling_) return;
    core::Context& context = ui();
    context.end_region();
    place_still(context.layout().last_item());
    scrolling_ = false;
}

bool ListEditor::begin_item(std::size_t index, bool selected) {
    core::Context& context = ui();
    Kit& k = kit();
    const Style& style = k.style;
    // A card's fields are not the row the list sits under.
    detail::set_edit_label(nullptr);
    index_ = index;
    selected_ = selected;
    clicked_ = false;
    context.ids().push(static_cast<int>(index));
    card_id_ = context.id("##card");
    card_min_ = cursor();
    card_width_ = space_left().x;

    // The background goes behind the fields, whose height is only known
    // once they are drawn: it is drawn at the height the card had last frame.
    const float row_h = frame_height();
    const float height = stored_float(card_id_, row_h + PAD * 2.0f);
    const glm::vec2 card_max{card_min_.x + card_width_, card_min_.y + height};

    // A card out of sight is passed over at the height it had.
    core::DrawList& dl = context.draw_list();
    if (card_max.y <= dl.clip_min().y || card_min_.y >= dl.clip_max().y) {
        set_cursor({card_min_.x, card_max.y + theme::GAP_S - style.item_spacing.y});
        reserve({card_width_, 0.0f});
        context.ids().pop();
        return false;
    }

    // A closed card that opens lifts under the mouse, to say it can be
    // clicked; its summary reads in full text then.
    const bool closed = options_.opens && !selected;
    const bool hovered = closed && mouse_over(card_min_, card_max);
    dl.rect_filled(card_min_, card_max,
                   faded(selected ? theme::selection() : hovered ? theme::surface_raised() : theme::surface_sunken()),
                   style.frame_rounding);

    // The card itself takes a click anywhere its fields don't: that selects
    // it. Everything drawn on it afterwards comes first.
    next_item_allows_overlap();
    const core::Rect card = context.layout().next({card_width_, height});
    clicked_ = place(context.id("##select"), card).clicked;

    // The grip: dragging it carries the item to another place in the list.
    const glm::vec2 line{card_min_.x, card_min_.y + PAD};
    if (options_.reorder) {
        const core::Rect grip{.min = {line.x + PAD_LEFT, line.y}, .max = {line.x + PAD_LEFT + GRIP_W, line.y + row_h}};
        const core::Interaction in = place(context.id("##grip"), grip);
        const bool grip_hot = in.hovered || in.held;
        if (grip_hot) set_mouse_cursor(MouseCursor::ResizeAll);
        if (context.drag_source()) {
            const Dragged dragged{list_id_, index};
            context.carry(PAYLOAD, &dragged, sizeof(dragged));
        }
        draw_text({grip.min.x + (GRIP_W - measure(icon::reorder).x) * 0.5f, line.y + style.frame_padding.y},
                  grip_hot ? theme::text_secondary() : theme::text_disabled(), icon::reorder);
    }

    // The number, counted from one, right-aligned on the first line's
    // baseline; in the accent on a selected card. A list that can't be
    // reordered has no grip, and the number takes its place.
    const float text_top = line.y + style.frame_padding.y;
    const float baseline = text_top + context.text().baseline(text_style());
    char number[16];
    std::snprintf(number, sizeof(number), "%zu", index + 1);
    const float grip_w = options_.reorder ? GRIP_W + theme::GAP_S : 0.0f;
    const float number_right = card_min_.x + PAD_LEFT + grip_w + INDEX_W;
    {
        FontScope small(Font::Current, theme::SMALL_TEXT);
        const float number_top = baseline - context.text().baseline(text_style());
        draw_text({number_right - measure(number).x, number_top}, selected ? theme::accent() : theme::text_disabled(),
                  number);
    }

    // A card that opens leads with a chevron, open on the selected card.
    float content_x = number_right + theme::GAP_S + theme::GAP_XS;
    if (options_.opens) {
        const char* chevron = selected ? icon::fold_open : icon::fold_closed;
        draw_text({content_x, text_top}, hovered ? theme::text_secondary() : theme::text_disabled(), chevron);
        content_x += measure(chevron).x + theme::GAP_S;
    }

    // The item's own fields, between the number and the trash icon, on the
    // darker field colour a sunken card needs; anything that sizes itself to
    // the room left ends at the card's content edge.
    const float content_w = std::max(card_min_.x + card_width_ - PAD - row_h - theme::GAP_S - content_x, 1.0f);
    const float right = content_x + content_w;
    core::Layout& layout = context.layout();
    layout.begin_area({.min = {content_x, line.y}, .max = {right, std::max(layout.area().max.y, line.y)}});
    // A first line of text (a card's summary or heading) sits on the field
    // line the number, grip and trash icon are centred on.
    align_text_to_frame();
    k.item_widths.push_back(content_w);
    k.frames.push_back({.rest = theme::surface_inset(), .hover = theme::surface_sunken(), .active = theme::surface_raised()});
    dl.push_clip({content_x, card_min_.y}, {right, std::numeric_limits<float>::max()});
    summary_colour_ = closed;
    if (summary_colour_) k.text_colours.push_back(hovered ? theme::text() : theme::text_secondary());
    return true;
}

bool ListEditor::end_item() {
    core::Context& context = ui();
    Kit& k = kit();
    if (summary_colour_) k.text_colours.pop_back();
    context.draw_list().pop_clip();
    k.frames.pop_back();
    k.item_widths.pop_back();
    k.next.text_on_frame = false;
    const glm::vec2 held = context.layout().end_area();
    const float row_h = frame_height();
    // At least one row tall, for the grip and the trash icon, padded both ends.
    const float height = std::max(held.y, row_h) + PAD * 2.0f;
    store_float(card_id_, height);

    // The trash icon, always there: removing is undoable.
    bool remove = false;
    if (options_.remove) {
        set_cursor({card_min_.x + card_width_ - PAD - row_h, card_min_.y + PAD});
        remove = icon_button("##remove", icon::remove, "Remove", ButtonKind::GhostDanger);
    }

    // The whole card takes a dragged item of this list, landing it before
    // or after this one by which half the pointer is over.
    const core::Rect card{.min = card_min_, .max = {card_min_.x + card_width_, card_min_.y + height}};
    if (options_.reorder) {
        if (const auto drop = context.drop_target(card, PAYLOAD)) {
            Dragged dragged{};
            if (drop->payload.size == sizeof(Dragged)) std::memcpy(&dragged, drop->payload.data, sizeof(Dragged));
            if (dragged.list == list_id_) {
                const bool after = mouse_pos().y > (card.min.y + card.max.y) * 0.5f;
                const std::size_t to = index_ + (after ? 1 : 0);
                const float y = after ? card.max.y + theme::GAP_XS : card.min.y - theme::GAP_XS;
                context.draw_list().rect_filled({card.min.x, y - INSERT_LINE * 0.5f}, {card.max.x, y + INSERT_LINE * 0.5f},
                                                faded(theme::accent()));
                if (drop->delivered && to != dragged.index && to != dragged.index + 1) {
                    moved_ = std::pair{dragged.index, to};
                }
            }
        }
    }

    // Below the card: an empty item placed so that the item spacing after
    // it leaves the gap between cards.
    set_cursor({card_min_.x, card_min_.y + height + theme::GAP_S - k.style.item_spacing.y});
    reserve({card_width_, 0.0f});
    context.ids().pop();
    return remove;
}

void ListEditor::empty(const char* text) {
    end_scroll();
    const glm::vec2 min = cursor();
    const float width = space_left().x;
    FontScope small(Font::Current, theme::SMALL_TEXT);
    const float wrap = width - theme::GAP_M * 2.0f;
    const glm::vec2 size = measure(text, wrap);
    const float height = size.y + theme::GAP_M * 2.0f + theme::GAP_XS;
    dashed_rect({.min = min, .max = {min.x + width, min.y + height}}, theme::surface_active());
    draw_text({min.x + std::max((width - size.x) * 0.5f, theme::GAP_M), min.y + (height - size.y) * 0.5f},
              theme::text_secondary(), text, wrap);
    reserve({width, height});
}

void card_heading(const char* icon, theme::Hue hue, const char* name, const char* tag_text) {
    if (icon != nullptr) {
        align_text_to_frame();
        text(icon, theme::hue(hue));
        same_line(theme::GAP_S + theme::GAP_XS);
    }
    align_text_to_frame();
    text(name, theme::text());
    if (tag_text == nullptr) return;
    // At the end of the line the card's fields end at.
    same_line(theme::GAP_M);
    const float room = space_left().x - tag_width(tag_text);
    if (room > 0.0f) set_cursor({cursor().x + room, cursor().y});
    tag(tag_text);
}

void card_summary(const char* text) {
    FontScope small(Font::Current, theme::SMALL_TEXT);
    ui::text(text);
}

bool ListEditor::add_button(const char* label) {
    end_scroll();
    const std::string text = std::string(icon::add) + "  " + label;
    return button(text.c_str(), ButtonKind::Ghost);
}

} // namespace ftk::ui
