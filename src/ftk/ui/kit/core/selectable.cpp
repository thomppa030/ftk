#include "ftk/ui/kit/selectable.hpp"

#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/text.hpp"

#include <glm/common.hpp>

namespace ftk::ui {

using namespace corekit;

bool selectable(const char* label, bool selected, const SelectableOptions& options) {
    core::Context& context = ui();
    core::Layout& layout = context.layout();
    const std::string_view text = options.text.empty() ? shown(label) : options.text;
    // In the mono face for code, or the part of it asked; the detail a gap
    // after it.
    const bool part = !options.code && options.code_size > 0 && options.code_at + options.code_size <= text.size();
    const std::string_view parts[3] = {
        part ? text.substr(0, options.code_at) : text,
        part ? text.substr(options.code_at, options.code_size) : std::string_view(),
        part ? text.substr(options.code_at + options.code_size) : std::string_view(),
    };
    // The parts side by side from `at`, drawn when `draw`: as wide as they
    // come to, as tall as the tallest.
    const auto lay_parts = [&](glm::vec2 at, bool draw) {
        glm::vec2 size{0.0f};
        // The first even when empty: a line of text tall all the same.
        for (std::size_t i = 0; i < 3; ++i) {
            if (i > 0 && parts[i].empty()) continue;
            FontScope face(options.code || i == 1 ? Font::Mono : Font::Current, 0.0f);
            const glm::vec2 one = measure(parts[i]);
            if (draw) draw_text({at.x + size.x, at.y}, corekit::text_colour(), parts[i]);
            size = {size.x + one.x, std::max(size.y, one.y)};
        }
        return size;
    };
    glm::vec2 measured = lay_parts({0.0f, 0.0f}, false);
    const float detail_at = measured.x + theme::GAP_M;
    if (!options.detail.empty()) {
        const glm::vec2 detail = measure(options.detail);
        measured = {detail_at + detail.x, std::max(measured.y, detail.y)};
    }
    // Across the room, or as wide as asked; a line of text tall, or as tall.
    // Unless asked for a width it is laid out as wide as its label, as
    // ImGui lays one out, so what follows on its line starts after the
    // label: a row of a list drawn after a selectable with none.
    const float width = options.size.x > 0.0f ? options.size.x : std::max(layout.space_left().x, measured.x);
    const float height = options.size.y > 0.0f ? options.size.y : measured.y;
    const core::Rect laid = layout.next({options.size.x > 0.0f ? width : measured.x, height});
    const core::Rect rect{.min = laid.min, .max = {laid.min.x + width, laid.max.y}};

    // It takes the mouse, and lights, across the spacing around it, so a
    // list of them has no gaps, as ImGui's do; across the whole of a
    // table's row when asked.
    const TableState* table = options.span_columns ? current_table() : nullptr;
    const glm::vec2 spacing{table != nullptr ? 0.0f : kit().style.item_spacing.x, kit().style.item_spacing.y};
    const glm::vec2 before = glm::floor(spacing * 0.5f);
    core::Rect bb{.min = rect.min - before, .max = rect.max + (spacing - before)};
    if (table != nullptr) {
        bb.min.x = table->rect.min.x;
        bb.max.x = table->rect.max.x;
    }

    if (options.overlap) kit().next.allow_overlap = true;
    const Id id = context.id(label);
    const core::Interaction in = place(id, bb, {});
    context.semantics(id, core::Role::ListItem, options.name.empty() ? text : options.name, {},
                      {.selected = selected}, rect);

    if (in.held || in.hovered || selected) {
        const Color fill = in.held ? theme::accent_dim() : (in.hovered ? theme::surface_hover() : theme::surface_raised());
        context.draw_list().rect_filled(bb.min, bb.max, faded(fill));
    }
    (void)lay_parts(rect.min, true);
    if (!options.detail.empty()) draw_text({rect.min.x + detail_at, rect.min.y}, theme::text_disabled(), options.detail);
    const bool picked = in.clicked || (options.double_click && in.double_clicked);
    // Picked in a popup, a drop-down's list or a menu, it closes it, unless
    // the list is one several are chosen from.
    if (picked && !options.keeps_popup_open && !kit().popup_stack.empty()) {
        context.close_popup(kit().popup_stack.back());
    }
    return picked;
}

} // namespace ftk::ui
