#include "ftk/ui/kit/rows_in_view.hpp"

#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/state.hpp"

#include <algorithm>
#include <cmath>

namespace ftk::ui {

using namespace corekit;

RowsInView::RowsInView(std::size_t count, RowPitch pitch) : count_{count}, pitch_{pitch} {
    // A row takes some room, or no row would ever be out of sight.
    pitch_.pitch = std::max(pitch_.pitch, 1.0f);
    start();
}

RowsInView::RowsInView(std::span<const float> tops)
    : count_{tops.empty() ? 0 : tops.size() - 1}, tops_{tops} {
    start();
}

RowsInView::~RowsInView() {
    finish();
}

void RowsInView::start() {
    core::Context& context = ui();
    live_id_ = context.id("##live_row");
    top_ = cursor().y;
    width_ = space_left().x;
    const core::DrawList& dl = context.draw_list();
    first_ = row_at(dl.clip_min().y - top_);
    end_ = std::max(std::min(row_at(dl.clip_max().y - top_) + 1, count_), first_);

    const int held = stored_int(live_id_, -1);
    const std::optional<std::size_t> kept =
        held >= 0 && static_cast<std::size_t>(held) < count_ ? std::optional(static_cast<std::size_t>(held))
                                                             : std::nullopt;
    rows_.reserve(end_ - first_ + 1);
    if (kept && *kept < first_) rows_.push_back(*kept);
    for (std::size_t i = first_; i < end_; ++i) rows_.push_back(i);
    if (kept && *kept >= end_) rows_.push_back(*kept);
    // The cursor waits at the first row drawn.
    if (!rows_.empty()) pass_over(rows_.front());
}

float RowsInView::top_of(std::size_t index) const {
    if (!tops_.empty()) return tops_[std::min(index, count_)];
    float top = static_cast<float>(index) * pitch_.pitch;
    if (pitch_.odd && *pitch_.odd < index) top += pitch_.odd_pitch - pitch_.pitch;
    return top;
}

std::size_t RowsInView::row_at(float offset) const {
    if (!tops_.empty()) {
        // The last row whose top is at or above the offset.
        const auto below = std::upper_bound(tops_.begin(), tops_.end(), offset);
        const auto index = std::max<std::ptrdiff_t>(below - tops_.begin() - 1, 0);
        return std::min(static_cast<std::size_t>(index), count_);
    }
    // Whole rows of the common pitch in `span`, as many as the run has at most.
    const auto rows_in = [&](float span) {
        return static_cast<std::size_t>(
            std::clamp(std::floor(span / pitch_.pitch), 0.0f, static_cast<float>(count_)));
    };
    if (pitch_.odd && *pitch_.odd < count_) {
        const float odd_top = top_of(*pitch_.odd);
        const float odd_end = odd_top + pitch_.odd_pitch;
        if (offset >= odd_end) return std::min(*pitch_.odd + 1 + rows_in(offset - odd_end), count_);
        if (offset >= odd_top) return *pitch_.odd;
    }
    return std::min(rows_in(offset), count_);
}

void RowsInView::pass_over(std::size_t index) {
    if (index <= next_) return;
    // An empty item placed so that the item spacing after it leaves the
    // cursor at the row's top.
    const glm::vec2 here = cursor();
    set_cursor({here.x, here.y + top_of(index) - top_of(next_) - kit().style.item_spacing.y});
    reserve({width_, 0.0f});
    next_ = index;
}

void RowsInView::close_row() {
    if (!drawing_) return;
    if (live_row() != live_before_) live_ = drawing_;
    kit().row_drawn = 0;
    drawing_.reset();
}

void RowsInView::at(std::size_t index) {
    close_row();
    placed_ = true;
    pass_over(index);
    next_ = index + 1;
    drawing_ = index;
    // What in the row takes the pointer or the keyboard names the run's
    // row, unless a row of its own inside names that.
    live_before_ = live_row();
    kit().row_drawn = live_id_;
}

void RowsInView::finish() {
    if (finished_) return;
    finished_ = true;
    close_row();
    // A loop without at() drew the rows in sight in order.
    if (!placed_) next_ = end_;
    pass_over(count_);
    store_int(live_id_, live_ ? static_cast<int>(*live_) : -1);
}

void RowsInView::reveal(std::size_t index) {
    if (index >= count_) return;
    core::Context& context = ui();
    const core::Rect shown = context.area_rect();
    const float row_top = top_ + top_of(index);
    const float row_bottom = top_ + top_of(index + 1);
    float by = 0.0f;
    if (row_top < shown.min.y) {
        by = row_top - shown.min.y;
    } else if (row_bottom > shown.max.y) {
        by = row_bottom - shown.max.y;
    }
    if (by != 0.0f) {
        context.set_scroll(context.scroll() + glm::vec2{0.0f, by});
        context.request_frame();
    }
}

} // namespace ftk::ui
