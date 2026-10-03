#include "ftk/ui/kit/plot.hpp"

#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/tooltip.hpp"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace ftk::ui {

using namespace corekit;

void plot_lines(const char* id, std::span<const float> values, int first, glm::vec2 size, PlotOptions options) {
    core::Context& context = ui();
    const Style& style = kit().style;
    // As ImGui sizes it: as wide as asked or as the next item, a field's
    // height unless asked.
    const core::Rect frame = context.layout().next({size.x > 0.0f ? size.x : item_width(),
                                                    size.y > 0.0f ? size.y : frame_height()});
    const Id plot_id = context.id(id);
    const core::Interaction in = place(plot_id, frame, {.left = false});
    core::DrawList& dl = context.draw_list();
    dl.rect_filled(frame.min, frame.max, faded(theme::surface_sunken()), style.frame_rounding);
    const core::Rect inner{.min = frame.min + style.frame_padding, .max = frame.max - style.frame_padding};
    const auto count = static_cast<int>(values.size());
    if (count >= 2 && inner.width() > 0.0f && inner.height() > 0.0f) {
        // From the least of the values to the most, unless asked.
        float lo = options.min;
        float hi = options.max.value_or(*std::ranges::max_element(values));
        if (hi <= lo) hi = lo + 1.0f;
        const auto value_at = [&](int i) { return values[static_cast<std::size_t>((i + first) % count)]; };
        std::vector<glm::vec2> points;
        points.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(count - 1);
            const float v = std::clamp((value_at(i) - lo) / (hi - lo), 0.0f, 1.0f);
            points.push_back({inner.min.x + inner.width() * t, inner.max.y - inner.height() * v});
        }
        dl.polyline(points, faded(in.hovered ? theme::success() : theme::accent()), 1.0f);
        // Under the mouse, the value there.
        if (in.hovered) {
            const float t = std::clamp((in.position.x - inner.min.x) / inner.width(), 0.0f, 0.9999f);
            const int i = static_cast<int>(t * static_cast<float>(count - 1) + 0.5f);
            char text[48];
            std::snprintf(text, sizeof(text), "%d: %8.4g", i, static_cast<double>(value_at(i)));
            tooltip(text);
        }
    }
    if (options.overlay != nullptr) {
        const glm::vec2 ts = measure(options.overlay);
        draw_text({frame.min.x + (frame.width() - ts.x) * 0.5f, frame.min.y + style.frame_padding.y},
                  corekit::text_colour(), options.overlay);
    }
}

} // namespace ftk::ui
