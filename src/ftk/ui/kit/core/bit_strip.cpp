#include "ftk/ui/kit/bit_strip.hpp"

#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/tooltip.hpp"

#include <cmath>
#include <cstdio>
#include <string>

namespace ftk::ui {

using namespace corekit;

namespace {

constexpr float SQUARE_W = 30.0f;
constexpr float SQUARE_GAP = 4.0f;
constexpr float SQUARE_ROUNDING = 4.0f;
constexpr float RING = 1.5f;
constexpr float LINE_GAP = 8.0f;
constexpr float NUMBER_W = 10.0f;

std::string numbered(std::size_t index) {
    char number[24];  // any size_t
    std::snprintf(number, sizeof(number), "%zu", index + 1);
    return number;
}

} // namespace

std::optional<std::size_t> bit_strip(const char* id, std::span<const Bit> bits) {
    std::optional<std::size_t> clicked;
    core::Context& context = ui();
    IdScope scope(id);
    const float h = frame_height();
    core::DrawList& dl = context.draw_list();
    for (std::size_t i = 0; i < bits.size(); ++i) {
        const Bit& bit = bits[i];
        if (i > 0) same_line(SQUARE_GAP);
        IdScope square(static_cast<int>(i));
        const core::Rect rect = context.layout().next({SQUARE_W, h});
        const Id bit_id = context.id("##bit");
        const core::Interaction in = place(bit_id, rect);
        const std::string number = numbered(i);
        context.semantics(bit_id, core::Role::Toggle, number, {},
                          {.checked = bit.state == BitState::on || bit.state == BitState::inherited});
        const bool settable = bit.state == BitState::off || bit.state == BitState::on;
        if (in.clicked && settable) clicked = i;
        if (in.hovered && bit.tooltip != nullptr) tooltip(bit.tooltip);

        Color ink = theme::text_secondary();
        switch (bit.state) {
            case BitState::on:
                dl.rect_filled(rect.min, rect.max, faded(theme::toggle_on()), SQUARE_ROUNDING);
                ink = theme::text();
                break;
            case BitState::inherited:
                dl.rect(rect.min, rect.max, faded(theme::toggle_on()), SQUARE_ROUNDING, RoundCorners::All, RING);
                ink = theme::text();
                break;
            case BitState::off:
                dl.rect_filled(rect.min, rect.max, faded(in.hovered ? theme::surface_hover() : theme::surface_sunken()),
                               SQUARE_ROUNDING);
                break;
            case BitState::unavailable:
                dl.rect(rect.min, rect.max, faded(theme::surface_raised()), SQUARE_ROUNDING);
                ink = theme::text_disabled();
                break;
        }
        FontScope mono(Font::Mono, theme::SMALL_TEXT);
        const glm::vec2 size = measure(number);
        draw_text({rect.min.x + std::floor((SQUARE_W - size.x) * 0.5f), rect.min.y + std::floor((h - size.y) * 0.5f)},
                  ink, number);
    }
    return clicked;
}

void bit_line(std::size_t index, const char* label, const char* from) {
    const std::string number = numbered(index);
    const glm::vec2 start = cursor();
    {
        // The number right-aligned in its column, mono and dim, and every
        // part of the line where a field's text would be.
        FontScope mono(Font::Mono, theme::SMALL_TEXT);
        set_cursor({start.x + NUMBER_W - text_size(number).x, start.y});
        align_text_to_frame();
        text(number, theme::text_disabled());
    }
    same_line(LINE_GAP);
    align_text_to_frame();
    text(label);
    if (from == nullptr) return;
    same_line(LINE_GAP);
    TextColour secondary(theme::text_secondary());
    align_text_to_frame();
    text(icon::inherited);
    same_line(4.0f);
    align_text_to_frame();
    text(std::string("from ") + from);
}

} // namespace ftk::ui
