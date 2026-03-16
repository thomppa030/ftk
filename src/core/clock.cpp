#include "core/clock.hpp"

namespace fjell {

void Clock::tick() {
    auto now = SteadyClock::now();
    delta_time_ = std::chrono::duration<float>(now - last_).count();
    elapsed_ = std::chrono::duration<float>(now - start_).count();
    last_ = now;
}

} // namespace fjell
