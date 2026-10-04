#pragma once

// A scrollbar down a scrolled view's edge (sheet 39): a dim track and a
// raised thumb as long as the share of the view that shows. Dragging the
// thumb scrolls; a press on the track takes the thumb there and drags it
// on. Marks on the track's right edge say where things are: problems,
// search hits, the caret. A window's own scrollbar is this one.
//
//     const ui::ScrollMark marks[] = {{.at = caret_y, .colour = theme::text()}};
//     if (ui::vertical_scrollbar("##scroll", track_min, track_max, scroll.y, height, room, marks)) ...;

#include "ftk/ui/types.hpp"

#include <glm/vec2.hpp>

#include <span>

namespace ftk::ui {

/// Something marked on a scrollbar's track: how far down it is, in the
/// units the view scrolls in, and its colour, a theme token.
struct ScrollMark {
    float at{0.0f};
    Color colour{};
};

/// A vertical scrollbar filling `track_min` to `track_max`, for a view
/// `visible` tall with `room` more of it below what shows. `offset` is how
/// far down the view is scrolled; the bar moves it, and returns true when
/// it did. Nothing is drawn when there is no room to scroll.
bool vertical_scrollbar(const char* id, glm::vec2 track_min, glm::vec2 track_max, float& offset, float visible,
                        float room, std::span<const ScrollMark> marks = {});

} // namespace ftk::ui
