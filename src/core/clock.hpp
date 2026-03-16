#pragma once

#include <chrono>

namespace fjell {

class Clock {
public:
    void tick();

    [[nodiscard]] float delta_time() const { return delta_time_; }
    [[nodiscard]] float elapsed() const { return elapsed_; }

private:
    using SteadyClock = std::chrono::steady_clock;
    using TimePoint = SteadyClock::time_point;

    TimePoint start_{SteadyClock::now()};
    TimePoint last_{start_};
    float delta_time_{0.0f};
    float elapsed_{0.0f};
};

} // namespace fjell
