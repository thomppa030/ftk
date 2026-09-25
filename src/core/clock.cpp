#include "core/clock.hpp"

#include <algorithm>

namespace fjell {

void Clock::tick() {
    auto now = SteadyClock::now();
    elapsed_ = std::chrono::duration<float>(now - start_).count();
    const float real_delta = std::chrono::duration<float>(now - last_).count();
    last_ = now;
    advance(real_delta);
}

void Clock::advance(float real_delta) {
    delta_time_ = real_delta;

    const float scaled = paused_ ? 0.0f : delta_time_ * time_scale_;
    game_delta_time_ = std::min(scaled, max_game_delta_);
    game_delta_dropped_ = scaled - game_delta_time_;
    game_elapsed_ += game_delta_time_;
}

} // namespace fjell
