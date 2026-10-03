#include "ftk/ui/kit/region.hpp"

#include "ftk/ui/kit/core/kit.hpp"

namespace ftk::ui {

using namespace corekit;

Region::Region(const char* id, glm::vec2 size, RegionOptions options) {
    core::Context& context = ui();
    // Zero across or down takes all the room there is that way.
    const glm::vec2 asked{size.x == 0.0f ? core::FILL : size.x, size.y == 0.0f ? core::FILL : size.y};
    context.begin_region(id, asked, {.wheel_scrolls = options.wheel_scrolls});
    const core::Rect rect = context.area_rect();
    if (options.border) context.draw_list().rect(rect.min, rect.max, faded(theme::border()));
    visible_ = !rect.empty();
}

Region::~Region() {
    core::Context& context = ui();
    context.end_region();
    // The region is the item just drawn, as a child window is in ImGui.
    place_still(context.layout().last_item());
}

} // namespace ftk::ui
