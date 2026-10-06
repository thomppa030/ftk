#include "ftk/ui/kit/memory_meter.hpp"

#include "ftk/base/string_utils.hpp"
#include "ftk/ui/draw_list.hpp"
#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/field.hpp"
#include "ftk/ui/kit/item.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/state.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/theme.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

namespace ftk::ui {

using namespace corekit;

namespace {

constexpr float TRACK = 20.0f;        // the track's height
constexpr float MARK = 2.0f;          // a cheaper level's mark in the fill
constexpr float HATCH_STEP = 7.0f;    // from one stripe to the next
constexpr float HATCH_WIDTH = 2.0f;   // a stripe
constexpr float SAVED_INSET = 8.0f;   // the saving's words from the track's end

// Stripes rising to the right over `min`..`max`.
void hatch(DrawList dl, glm::vec2 min, glm::vec2 max) {
    if (max.x <= min.x) return;
    dl.push_clip(min, max);
    const float height = max.y - min.y;
    for (float x = min.x - height; x < max.x; x += HATCH_STEP) {
        dl.line({x, max.y}, {x + height, min.y}, faded(theme::hatch()), HATCH_WIDTH);
    }
    dl.pop_clip();
}

} // namespace

std::size_t memory_meter(const char* id, const MemoryMeterSpec& spec) {
    if (spec.levels.empty()) return 0;
    const IdScope scope(id);
    const Id hovered_id = ui::id("##hovered");
    const std::size_t count = spec.levels.size();
    const std::size_t in_use = std::min(spec.in_use, count - 1);
    // The level shown is the one hovered when the levels were last drawn,
    // under the meter, else the one in use.
    const int hovered = stored_int(hovered_id, -1);
    const std::size_t shown = hovered >= 0 && static_cast<std::size_t>(hovered) < count
                                  ? static_cast<std::size_t>(hovered)
                                  : in_use;
    const MeterLevel& level = spec.levels[shown];
    const float width = std::max(space_left().x, 1.0f);
    DrawList dl = DrawList::window();

    // The cost in large type, and the level it is at, their baselines
    // together: a face's ascent is about four-fifths of its size.
    const glm::vec2 head = cursor();
    {
        const FontScope figure(theme::bold_font(), theme::FIGURE_TEXT);
        text(size_text(level.bytes));
    }
    same_line(theme::GAP_M);
    set_cursor({cursor().x, head.y + (theme::FIGURE_TEXT - theme::BODY_TEXT) * 0.8f});
    const bool at_use = shown == in_use && spec.in_use_note[0] != '\0';
    text(at_use ? std::format("at {}, {}", level.name, spec.in_use_note) : std::format("at {}", level.name),
         theme::text_secondary());
    set_cursor({head.x, std::max(cursor().y, head.y + theme::FIGURE_TEXT + theme::GAP_S)});

    // The track is the whole; the fill, what it costs at the level shown.
    const glm::vec2 track_min = cursor();
    const glm::vec2 track_max{track_min.x + width, track_min.y + TRACK};
    reserve({width, TRACK});
    const double whole = static_cast<double>(std::max<uint64_t>(spec.whole, 1));
    const auto x_of = [&](uint64_t bytes) {
        return track_min.x + width * static_cast<float>(std::clamp(static_cast<double>(bytes) / whole, 0.0, 1.0));
    };
    const float rounding = kit().style.frame_rounding;
    const float fill_x = x_of(level.bytes);
    dl.rect_filled(track_min, track_max, faded(theme::surface_sunken()), rounding);
    hatch(dl, {fill_x, track_min.y}, track_max);
    if (fill_x > track_min.x) {
        dl.rect_filled(track_min, {fill_x, track_max.y}, faded(theme::text_secondary()), rounding);
    }
    for (const MeterLevel& cheaper : spec.levels) {
        if (cheaper.bytes >= level.bytes) continue;
        const float x = x_of(cheaper.bytes);
        dl.rect_filled({x - MARK * 0.5f, track_min.y}, {x + MARK * 0.5f, track_max.y}, faded(theme::surface_sunken()));
    }
    // What the saving comes to, in the hatched part when it fits there.
    if (spec.whole > level.bytes) {
        const uint64_t saved = spec.whole - level.bytes;
        const std::string words = std::format("{} saved \xc2\xb7 {} %", size_text(saved),
                                              std::lround(100.0 * static_cast<double>(saved) / whole));
        const FontScope small(Font::Current, theme::SMALL_TEXT);
        const glm::vec2 size = measure(words);
        if (size.x + SAVED_INSET * 2.0f <= track_max.x - fill_x) {
            draw_text({track_max.x - SAVED_INSET - size.x, track_min.y + (TRACK - size.y) * 0.5f},
                      theme::text_secondary(), words);
        }
    }

    // The scale's ends under the track.
    {
        const FontScope mono(theme::mono_font(), theme::AXIS_TEXT);
        const std::string end = std::format("{} {}", size_text(spec.whole), spec.whole_name);
        const glm::vec2 row = cursor();
        text("0", theme::text_disabled());
        same_line(width - measure("0").x - measure(end).x);
        text(end, theme::text_disabled());
        set_cursor({row.x, cursor().y});
    }

    // Each level and its cost on one line, the one in use tagged; the one
    // under the mouse is shown above from the next frame.
    int over = -1;
    for (std::size_t i = 0; i < count; ++i) {
        const MeterLevel& each = spec.levels[i];
        const std::string cost = size_text(each.bytes);
        const IdScope level_id(static_cast<int>(i));
        if (i > 0) same_line(theme::GAP_L);
        const glm::vec2 from = cursor();
        const float name_w = measure(each.name).x;
        const float cost_w = measure(cost).x;
        const bool tagged = i == in_use && spec.in_use_tag[0] != '\0';
        const float tag_w = tagged ? tag_width(spec.in_use_tag) + theme::GAP_S : 0.0f;
        (void)hit_area("##level", {name_w + theme::GAP_S + cost_w + tag_w, frame_height()});
        if (item_hovered()) over = static_cast<int>(i);
        set_cursor(from);
        align_text_to_frame();
        text(each.name, theme::text_secondary());
        same_line(theme::GAP_S);
        text(cost);
        if (tagged) {
            same_line(theme::GAP_S);
            tag(spec.in_use_tag);
        }
    }
    store_int(hovered_id, over);
    return shown;
}

} // namespace ftk::ui
