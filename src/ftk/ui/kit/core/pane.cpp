#include "ftk/ui/kit/pane.hpp"

#include "ftk/ui/kit/core/kit.hpp"

namespace ftk::ui {

using namespace corekit;

Pane::Pane(const char* id, glm::vec2 size, PaneSurface surface) {
    core::Context& context = ui();
    // Zero across or down takes all the room there is that way, as a child
    // window does in ImGui.
    const glm::vec2 asked{size.x == 0.0f ? core::FILL : size.x, size.y == 0.0f ? core::FILL : size.y};
    context.begin_region(id, asked, {.padding = {theme::GAP_L, theme::GAP_L}});
    const core::Rect rect = context.area_rect();
    context.draw_list().rect_filled(
        rect.min, rect.max, faded(surface == PaneSurface::Sunken ? theme::surface_sunken() : theme::surface_base()));
    visible_ = !rect.empty();
}

Pane::~Pane() {
    core::Context& context = ui();
    context.end_region();
    place_still(context.layout().last_item());
}

} // namespace ftk::ui
