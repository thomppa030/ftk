#include "ftk/ui/kit/rail.hpp"

#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/text.hpp"

#include <cmath>
#include <optional>

namespace ftk::ui {

using namespace corekit;

namespace {

// The line runs this far in from the left, and the rows start past it.
constexpr float LINE_X = 8.5f;
constexpr float INDENT = 22.0f;
constexpr float DOT = 7.0f;
constexpr float STEP_H = 24.0f;
constexpr float ITEM_H = 26.0f;
constexpr float ITEM_PAD = 8.0f;
constexpr float ROUNDING = 4.0f;

} // namespace

std::optional<std::size_t> rail(const char* id, std::span<const RailEntry> entries, theme::Hue hue) {
    std::optional<std::size_t> clicked;
    if (entries.empty()) return clicked;
    core::Context& context = ui();
    IdScope scope(id);
    core::DrawList& dl = context.draw_list();
    const glm::vec2 origin = cursor();
    const float width = space_left().x;
    auto height_of = [](const RailEntry& e) { return e.item ? ITEM_H : STEP_H; };

    // The line, behind the dots: from the first row's middle to the last's.
    float total = 0.0f;
    for (const RailEntry& e : entries) total += height_of(e);
    const float line_x = std::floor(origin.x + LINE_X);
    dl.rect_filled({line_x, origin.y + height_of(entries.front()) * 0.5f},
                   {line_x + 1.0f, origin.y + total - height_of(entries.back()) * 0.5f},
                   faded(theme::surface_highest()));

    const Color hue_colour = theme::hue(hue);
    float y = origin.y;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const RailEntry& e = entries[i];
        const float h = height_of(e);
        const float mid = y + h * 0.5f;
        const core::Rect row{.min = {origin.x + INDENT, y}, .max = {origin.x + width, y + h}};
        IdScope entry(static_cast<int>(i));

        float text_x = row.min.x;
        if (e.item) {
            const Id item_id = context.id("##item");
            const core::Interaction in = place(item_id, row);
            context.semantics(item_id, core::Role::ListItem, e.label, {}, {.selected = e.selected});
            if (in.clicked) clicked = e.item;
            if (e.selected || in.hovered) {
                dl.rect_filled(row.min, row.max, faded(e.selected ? theme::selection() : theme::surface_hover()),
                               ROUNDING);
            }
            text_x += ITEM_PAD;
            if (e.icon != nullptr) {
                const glm::vec2 size = measure(e.icon);
                draw_text({text_x, std::floor(mid - size.y * 0.5f)}, hue_colour, e.icon);
                text_x += size.x + theme::GAP_S + theme::GAP_XS;
            }
        }
        dl.circle_filled({line_x + 0.5f, mid}, DOT * 0.5f, faded(e.item ? hue_colour : theme::surface_highest()));

        // A step is quieter than an item: small and dim.
        glm::vec2 label{0.0f};
        {
            std::optional<FontScope> small;
            if (!e.item) small.emplace(Font::Current, theme::SMALL_TEXT);
            label = measure(e.label);
            draw_text({text_x, std::floor(mid - label.y * 0.5f)}, e.item ? theme::text() : theme::text_disabled(),
                      e.label);
        }
        if (e.note != nullptr) {
            FontScope small(Font::Current, theme::SMALL_TEXT);
            const glm::vec2 note = measure(e.note);
            draw_text({text_x + label.x + theme::GAP_S + theme::GAP_XS, std::floor(mid - note.y * 0.5f)},
                      theme::text_secondary(), e.note);
        }
        y += h;
    }
    set_cursor(origin);
    reserve({width, total});
    return clicked;
}

} // namespace ftk::ui
