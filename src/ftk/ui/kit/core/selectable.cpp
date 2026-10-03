#include "ftk/ui/kit/selectable.hpp"

#include "ftk/ui/kit/core/kit.hpp"

namespace ftk::ui {

using namespace corekit;

bool selectable(const char* label, bool selected, const SelectableOptions& options) {
    core::Context& context = ui();
    core::Layout& layout = context.layout();
    const std::string_view text = shown(label);
    const glm::vec2 measured = measure(text);
    // Across the room, or as wide as asked; a line of text tall, or as tall.
    const float width = options.size.x > 0.0f ? options.size.x : std::max(layout.space_left().x, measured.x);
    const float height = options.size.y > 0.0f ? options.size.y : measured.y;
    const core::Rect rect = layout.next({width, height});
    if (options.overlap) kit().next.allow_overlap = true;
    const Id id = context.id(label);
    const core::Interaction in = place(id, rect, {});
    context.semantics(id, core::Role::ListItem, text, {}, {.selected = selected});

    // Lit across the spacing around it, so a list of them has no gaps.
    const glm::vec2 half = kit().style.item_spacing * 0.5f;
    const glm::vec2 min = rect.min - glm::vec2{0.0f, half.y};
    const glm::vec2 max = rect.max + glm::vec2{0.0f, half.y};
    if (in.held || in.hovered || selected) {
        const Color fill = in.held ? theme::accent_dim() : (in.hovered ? theme::surface_hover() : theme::surface_raised());
        context.draw_list().rect_filled(min, max, faded(fill));
    }
    draw_text(rect.min, corekit::text_colour(), text);
    const bool picked = in.clicked || (options.double_click && in.double_clicked);
    // Picked in a popup, a drop-down's list or a menu, it closes it.
    if (picked && !kit().popup_stack.empty()) context.close_popup(kit().popup_stack.back());
    return picked;
}

} // namespace ftk::ui
