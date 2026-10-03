#include "ftk/ui/kit/loading.hpp"

#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/text.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <string>

namespace ftk::ui {

using namespace corekit;

void spinner(DrawList list, glm::vec2 centre, float radius, float seconds) {
    constexpr float TURN = 4.0f;  // radians a second
    const float start = seconds * TURN;
    list.arc(centre, radius, start, start + std::numbers::pi_v<float> * 1.4f, theme::accent(), 2.5f);
}

void loading(DrawList list, glm::vec2 top_centre, std::string_view what, float seconds) {
    constexpr float RADIUS = 14.0f;
    spinner(list, {top_centre.x, top_centre.y + RADIUS}, RADIUS, seconds);
    const float w = measure(what).x;
    list.text({std::floor(top_centre.x - w * 0.5f), top_centre.y + RADIUS * 2.0f + theme::GAP_L},
              theme::text_secondary(), what);
}

void progress_bar(float fraction, float width, const char* label) {
    core::Context& context = ui();
    const Style& style = kit().style;
    // As ImGui sizes it: as wide as asked, the room less as much below
    // zero, the next item's width at zero; a field's height.
    const float room = context.layout().space_left().x;
    const float w = width > 0.0f ? width : width < 0.0f ? std::max(room + width, 4.0f) : item_width();
    const core::Rect rect = context.layout().next({w, frame_height()});
    place_still(rect);

    fraction = std::clamp(fraction, 0.0f, 1.0f);
    core::DrawList& dl = context.draw_list();
    dl.rect_filled(rect.min, rect.max, faded(theme::surface_sunken()), style.frame_rounding);
    const float fill = rect.min.x + rect.width() * fraction;
    if (fraction > 0.0f) {
        dl.rect_filled(rect.min, {fill, rect.max.y}, faded(theme::accent()), style.frame_rounding);
    }
    // The fraction as a percentage unless something else is said, after the
    // fill and inside the bar.
    std::string text;
    if (label != nullptr) {
        text = label;
    } else {
        char percent[32];
        std::snprintf(percent, sizeof(percent), "%.0f%%", fraction * 100.0f + 0.01f);
        text = percent;
    }
    const glm::vec2 size = measure(text);
    if (size.x <= 0.0f) return;
    const float x = std::clamp(fill + style.item_spacing.x, rect.min.x, rect.max.x - size.x - style.item_inner_spacing.x);
    dl.push_clip(rect.min, rect.max);
    draw_text({x, rect.min.y + (rect.height() - size.y) * 0.5f}, corekit::text_colour(), text);
    dl.pop_clip();
}

} // namespace ftk::ui
