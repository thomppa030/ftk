#include "core/fixed_timestep.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace fjell {

void FixedTimestep::reset(double step_seconds, double start_time) {
    assert(step_seconds > 0.0);
    dt_ = step_seconds;
    start_time_ = start_time;
    accumulator_ = 0.0;
    index_ = 0;
    pending_ = 0;
    single_step_ = false;
}

int FixedTimestep::advance(double frame_seconds) {
    assert(pending_ == 0 && "advance() before the last frame's steps were all taken");

    if (single_step_) {
        single_step_ = false;
        pending_ = 1;
        return pending_;
    }

    accumulator_ += std::max(frame_seconds, 0.0);
    const auto whole = static_cast<int>(std::floor(accumulator_ / dt_));
    // Frame time arrives limited to MAX_STEPS_PER_FRAME steps, and the
    // remainder carried in is under one, so this bound only catches the last
    // rounding of the two together. What it holds back stays in the
    // accumulator and runs next frame.
    pending_ = std::min(whole, MAX_STEPS_PER_FRAME);
    accumulator_ -= static_cast<double>(pending_) * dt_;
    return pending_;
}

FixedStepTime FixedTimestep::next_step() {
    assert(pending_ > 0 && "next_step() beyond the steps advance() returned");
    --pending_;
    FixedStepTime step{.dt = dt_, .time = time(), .index = index_};
    ++index_;
    return step;
}

double FixedTimestep::alpha() const {
    return std::clamp(accumulator_ / dt_, 0.0, std::nextafter(1.0, 0.0));
}

} // namespace fjell
