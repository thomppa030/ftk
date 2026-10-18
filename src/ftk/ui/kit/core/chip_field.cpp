#include "ftk/ui/kit/chip_field.hpp"

#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/tooltip.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace ftk::ui {

using namespace corekit;

namespace {

// The body text, face and size, on a neutral grey (kit spec, Tags: option A).
constexpr float CHIP_TEXT = theme::BODY_TEXT;
constexpr float CHIP_PADDING = 8.0f;
// The × a hovered chip shows in its right end, as wide as that end.
constexpr float CROSS_END = 18.0f;
// Between chips, and between the chips and the field's edge.
constexpr float CHIP_GAP = theme::GAP_S;
constexpr float FIELD_PADDING = theme::GAP_XS;

} // namespace

ChipEdit chip_field(const char* id, std::span<const Chip> chips, const char* label) {
    core::Context& context = ui();
    core::DrawList& draw = context.draw_list();
    const float rounding = kit().style.frame_rounding;
    IdScope scope(id);
    ChipEdit edit;

    std::vector<float> widths;
    float text_height = 0.0f;
    {
        FontScope body(Font::Body, CHIP_TEXT);
        text_height = line_height();
        for (const Chip& chip : chips) widths.push_back(measure(chip.text).x + (CHIP_PADDING * 2.0f));
    }
    const float line = std::max(frame_height() - (FIELD_PADDING * 2.0f), text_height);
    const float width = space_left().x;

    // The chips laid along lines, wrapping where the next one would pass the
    // room left for the chevron at the field's end.
    const float room = width - (FIELD_PADDING * 2.0f) - line - CHIP_GAP;
    std::vector<glm::vec2> at;
    glm::vec2 pen{FIELD_PADDING, FIELD_PADDING};
    for (const float chip_width : widths) {
        if (pen.x > FIELD_PADDING && pen.x + chip_width > FIELD_PADDING + room) pen = {FIELD_PADDING, pen.y + line + CHIP_GAP};
        at.push_back(pen);
        pen.x += chip_width + CHIP_GAP;
    }
    const float height = pen.y + line + FIELD_PADDING;
    const core::Rect field = context.layout().next({width, height});
    const Id field_id = context.id("##field");
    // The chips placed over it afterwards take the mouse where they are.
    kit().next.allow_overlap = true;
    const core::Interaction in = place(field_id, field);
    context.semantics(field_id, core::Role::Choice, label, chips.empty() ? "None" : "", {}, field);
    bool hovered = in.hovered;
    bool clicked = in.clicked;
    // The fill is drawn once the chips are, when whether one of them is
    // hovered is known, and put beneath them.
    const std::size_t chips_drawn = draw.mark();

    for (std::size_t i = 0; i < chips.size(); ++i) {
        const Chip& chip = chips[i];
        IdScope chip_scope(static_cast<int>(i));
        const glm::vec2 min = field.min + at[i];
        const core::Rect rect{.min = min, .max = min + glm::vec2{widths[i], line}};
        const Id chip_id = context.id("##chip");
        // The × placed over its end afterwards takes the mouse there.
        kit().next.allow_overlap = true;
        const core::Interaction over = place(chip_id, rect);
        context.semantics(chip_id, core::Role::ListItem, chip.text, {}, {}, rect);
        const bool problem = !chip.problem.empty();
        if (problem) item_tooltip(chip.problem);

        const core::Rect cross_rect{.min = {rect.max.x - CROSS_END, rect.min.y}, .max = rect.max};
        const Id cross_id = context.id("##take_off");
        const core::Interaction take = place(cross_id, cross_rect);
        const std::string take_label = "Take off " + std::string(chip.text);
        context.semantics(cross_id, core::Role::Button, take_label, {}, {}, cross_rect);
        if (take.clicked) edit.removed = i;
        hovered |= over.hovered || take.hovered;
        clicked |= over.clicked;

        draw.rect_filled(rect.min, rect.max, faded(problem ? theme::danger() : theme::surface_active()), rounding);
        const Color ink = problem ? theme::text_on_danger() : theme::text();
        {
            FontScope body(Font::Body, CHIP_TEXT);
            const float text_y = rect.min.y + std::floor((line - text_height) * 0.5f);
            draw_text({rect.min.x + CHIP_PADDING, text_y}, ink, chip.text);
        }
        // Hovered, the chip's end turns into its ×, over the end of the name.
        if (over.hovered || take.hovered) {
            if (!problem) draw.rect_filled(cross_rect.min, cross_rect.max, faded(theme::surface_highest()), rounding);
            const glm::vec2 cross = measure(icon::clear);
            draw_text(cross_rect.min + ((cross_rect.max - cross_rect.min) - cross) * 0.5f,
                      take.hovered ? ink : (problem ? ink : theme::text_secondary()), icon::clear);
        }
    }

    const FrameColours colours = frame_colours();
    const std::size_t fill = draw.mark();
    draw.rect_filled(field.min, field.max, faded(hovered ? colours.hover : colours.rest), rounding);
    draw.put_under(chips_drawn, fill);
    if (chips.empty()) {
        FontScope body(Font::Body, CHIP_TEXT);
        draw_text({field.min.x + CHIP_PADDING, field.min.y + FIELD_PADDING + std::floor((line - text_height) * 0.5f)},
                  theme::text_disabled(), "None");
    }
    // The chevron at the end of the first line, by the row's label.
    const glm::vec2 chevron = measure(icon::dropdown);
    draw_text({field.max.x - FIELD_PADDING - line + ((line - chevron.x) * 0.5f),
               field.min.y + FIELD_PADDING + ((line - chevron.y) * 0.5f)},
              theme::text_secondary(), icon::dropdown);
    if (clicked && !edit.removed) edit.open = glm::vec2{field.min.x, field.max.y};
    return edit;
}

} // namespace ftk::ui
