#pragma once

#include <chrono>

namespace fjell {

class Clock {
public:
    void tick();

    /// Raw wall-clock delta (for rendering, physics, input).
    [[nodiscard]] float delta_time() const { return delta_time_; }

    /// Scaled delta for gameplay (respects time_scale and pause).
    [[nodiscard]] float game_delta_time() const { return game_delta_time_; }

    /// Raw wall-clock elapsed since start.
    [[nodiscard]] float elapsed() const { return elapsed_; }

    /// Scaled elapsed gameplay time (accumulates only while unpaused).
    [[nodiscard]] float game_elapsed() const { return game_elapsed_; }

    void set_time_scale(float scale) { time_scale_ = scale; }
    [[nodiscard]] float time_scale() const { return time_scale_; }

    void set_paused(bool paused) { paused_ = paused; }
    [[nodiscard]] bool is_paused() const { return paused_; }

private:
    using SteadyClock = std::chrono::steady_clock;
    using TimePoint = SteadyClock::time_point;

    TimePoint start_{SteadyClock::now()};
    TimePoint last_{start_};
    float delta_time_{0.0f};
    float game_delta_time_{0.0f};
    float elapsed_{0.0f};
    float game_elapsed_{0.0f};
    float time_scale_{1.0f};
    bool paused_{false};
};

} // namespace fjell
