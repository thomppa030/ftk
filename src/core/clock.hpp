#pragma once

#include <chrono>
#include <limits>

namespace fjell {

class Clock {
public:
    /// Measure the time since the last tick and advance by it.
    void tick();

    /// Advance by a measured wall-clock delta: what `tick()` does once it has
    /// measured, apart from the wall-clock elapsed time.
    void advance(float real_delta);

    /// Raw wall-clock delta (for rendering, physics, input).
    [[nodiscard]] float delta_time() const { return delta_time_; }

    /// Scaled delta for gameplay (respects time_scale, pause and the limit).
    [[nodiscard]] float game_delta_time() const { return game_delta_time_; }

    /// Raw wall-clock elapsed since start.
    [[nodiscard]] float elapsed() const { return elapsed_; }

    /// Scaled elapsed gameplay time (accumulates only while unpaused).
    [[nodiscard]] float game_elapsed() const { return game_elapsed_; }

    void set_time_scale(float scale) { time_scale_ = scale; }
    [[nodiscard]] float time_scale() const { return time_scale_; }

    void set_paused(bool paused) { paused_ = paused; }
    [[nodiscard]] bool is_paused() const { return paused_; }

    /// The most game time one frame may count for. A frame that took longer
    /// (a hitch, a debugger stop, a load) counts for this long, so the whole
    /// game slows down through it together rather than the simulation trying
    /// to catch it all up at once. Unlimited until set; play sets it from the
    /// fixed step.
    void set_max_game_delta(float seconds) { max_game_delta_ = seconds; }
    void clear_max_game_delta() { max_game_delta_ = std::numeric_limits<float>::infinity(); }

    /// Whether the limit cut this frame's game delta short.
    [[nodiscard]] bool game_delta_limited() const { return game_delta_limited_; }

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
    float max_game_delta_{std::numeric_limits<float>::infinity()};
    bool paused_{false};
    bool game_delta_limited_{false};
};

} // namespace fjell
