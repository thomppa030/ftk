#include "core/clock.hpp"

namespace fjell {

void Clock::tick() {
    auto now = SteadyClock::now();
    delta_time_ = std::chrono::duration<float>(now - last_).count();
    elapsed_ = std::chrono::duration<float>(now - start_).count();
    last_ = now;

    game_delta_time_ = paused_ ? 0.0f : delta_time_ * time_scale_;
    game_elapsed_ += game_delta_time_;
}

} // namespace fjell
