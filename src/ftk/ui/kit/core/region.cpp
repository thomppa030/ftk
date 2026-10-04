#include "ftk/ui/kit/region.hpp"

#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/state.hpp"

#include <algorithm>

namespace ftk::ui {

using namespace corekit;

namespace {
// The strip either side of a resizing region's right edge that drags it,
// and the narrowest it is let go.
constexpr float HANDLE = 3.0f;
constexpr float MIN_WIDTH = 60.0f;
} // namespace

Region::Region(const char* id, glm::vec2 size, RegionOptions options) {
    core::Context& context = ui();
    // Zero across or down takes all the room there is that way; a region
    // that resizes is as wide as it was left.
    glm::vec2 asked{size.x == 0.0f ? core::FILL : size.x, size.y == 0.0f ? core::FILL : size.y};
    if (options.resize_x && size.x > 0.0f) {
        width_id_ = core::hash_label("##width", context.id(id));
        asked.x = stored_float(width_id_, size.x);
    }
    context.begin_region(id, asked, {.wheel_scrolls = options.wheel_scrolls});
    const core::Rect rect = context.area_rect();
    min_ = rect.min;
    max_ = rect.max;
    if (options.border) context.draw_list().rect(rect.min, rect.max, faded(theme::border()));
    visible_ = !rect.empty();
}

Region::~Region() {
    core::Context& context = ui();
    context.end_region();
    // The region is the item just drawn, as a child window is in ImGui.
    const core::Rect held = context.layout().last_item();
    if (width_id_ != 0) {
        // Its right edge, dragged from where it was when pressed.
        const Id handle_id = core::hash_label("##edge", width_id_);
        const Id from_id = core::hash_label("##from", width_id_);
        const core::Rect handle{.min = {max_.x - HANDLE, min_.y}, .max = {max_.x + HANDLE, max_.y}};
        const core::Interaction in = context.interact(handle_id, handle, {.beneath = true});
        if (in.hovered || in.held) context.set_cursor_shape(MouseCursor::ResizeEW);
        if (in.pressed) store_float(from_id, max_.x - min_.x);
        if (in.held && in.dragging) {
            const float width = stored_float(from_id, max_.x - min_.x) + in.position.x - in.press_position.x;
            store_float(width_id_, std::max(width, MIN_WIDTH));
            context.request_frame();
        }
    }
    place_still(held);
}

} // namespace ftk::ui
