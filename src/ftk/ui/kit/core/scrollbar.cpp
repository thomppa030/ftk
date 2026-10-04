#include "ftk/ui/kit/scrollbar.hpp"

#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/state.hpp"

#include <algorithm>

namespace ftk::ui {

using namespace corekit;

namespace {
// The thumb sits this far in from the track's edges; a mark is this wide
// and tall at the track's right edge.
constexpr float THUMB_INSET = 2.0f;
constexpr glm::vec2 MARK{4.0f, 2.0f};
} // namespace

namespace corekit {

bool scrollbar_at(Id id, core::Rect track, float& offset, float visible, float room, std::span<const ScrollMark> marks) {
    if (room <= 0.0f || track.height() <= 0.0f || track.width() <= 0.0f) return false;
    core::Context& context = ui();
    const Style& style = kit().style;
    const float grab = std::max(track.height() * visible / (visible + room), style.scrollbar_size * 2.0f);
    const float travel = std::max(track.height() - grab, 1.0f);
    const auto thumb_at = [&](float scrolled) {
        const float top = track.min.y + travel * std::clamp(scrolled, 0.0f, room) / room;
        return core::Rect{.min = {track.min.x, top}, .max = {track.max.x, top + grab}};
    };

    // The thumb over the track: a press on the thumb drags it from where it
    // was, one on the track takes the thumb's middle there first.
    const Id thumb_id = core::hash_label("##thumb", id);
    const Id grabbed_id = core::hash_label("##grabbed", id);
    const core::Interaction on_thumb = context.interact(thumb_id, thumb_at(offset), {.beneath = true});
    const core::Interaction on_track = context.interact(core::hash_label("##track", id), track, {.beneath = true});
    const float before = offset;
    if (on_track.pressed) {
        offset = std::clamp((on_track.press_position.y - track.min.y - grab * 0.5f) / travel * room, 0.0f, room);
        store_float(grabbed_id, offset);
    }
    if (on_thumb.pressed) store_float(grabbed_id, offset);
    const core::Interaction& held = on_thumb.held ? on_thumb : on_track;
    if (held.held) {
        const float moved = (held.position.y - held.press_position.y) / travel * room;
        offset = std::clamp(stored_float(grabbed_id, offset) + moved, 0.0f, room);
    }

    core::DrawList& dl = context.draw_list();
    dl.rect_filled(track.min, track.max, faded(theme::surface_sunken() * glm::vec4{1.0f, 1.0f, 1.0f, 0.5f}));
    const core::Rect thumb = thumb_at(offset);
    const bool hovered = on_thumb.hovered || held.held;
    const Color colour = held.held ? theme::surface_active() : (hovered ? theme::surface_hover() : theme::surface_raised());
    dl.rect_filled(thumb.min + glm::vec2{THUMB_INSET}, thumb.max - glm::vec2{THUMB_INSET}, faded(colour),
                   style.scrollbar_size * 0.5f);
    for (const ScrollMark& mark : marks) {
        const float y = track.min.y + track.height() * mark.at / (visible + room);
        dl.rect_filled({track.max.x - MARK.x, y}, {track.max.x, y + MARK.y}, faded(mark.colour));
    }
    return offset != before;
}

} // namespace corekit

bool vertical_scrollbar(const char* id, glm::vec2 track_min, glm::vec2 track_max, float& offset, float visible,
                        float room, std::span<const ScrollMark> marks) {
    return scrollbar_at(ui().id(id), {.min = track_min, .max = track_max}, offset, visible, room, marks);
}

} // namespace ftk::ui
