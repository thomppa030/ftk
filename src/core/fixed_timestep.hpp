#pragma once

#include <cstdint>

namespace fjell {

/// One fixed step as the simulation sees it.
struct FixedStepTime {
    /// The step's length, the same for every step of a session.
    double dt{0.0};
    /// Simulation time when the step starts.
    double time{0.0};
    /// How many steps this session took before this one.
    std::uint64_t index{0};
};

/// The simulation's clock. Frames take however long they take; the
/// simulation advances in steps of one fixed length, as many per frame as the
/// frame's game time covers, and carries the remainder to the next frame.
///
/// Every step having the same length is what makes the simulation behave the
/// same at any frame rate and replay exactly from the same inputs. Frame time
/// arrives already limited (see `max_frame_seconds()`), so a frame never asks
/// for more than a handful of steps.
class FixedTimestep {
public:
    /// Steps per second a project runs at unless it sets its own
    /// (`ProjectConfig::SimulationConfig::fixed_tick_rate`), and the range it
    /// may set.
    static constexpr std::uint32_t DEFAULT_TICK_RATE = 60;
    static constexpr std::uint32_t MIN_TICK_RATE = 10;
    static constexpr std::uint32_t MAX_TICK_RATE = 240;

    /// The most steps one frame runs. A frame is not allowed to count for
    /// more game time than this many steps cover: the clock limits it before
    /// it gets here, so a hitch slows the game down instead of piling up
    /// steps that make the next frame slower still.
    static constexpr int MAX_STEPS_PER_FRAME = 4;

    /// Start a session: no steps taken, nothing carried over, simulation time
    /// at `start_time`.
    void reset(double step_seconds, double start_time);

    /// Feed one frame's game time in and return how many steps it covers,
    /// usually 0, 1 or 2. Call `next_step()` that many times.
    [[nodiscard]] int advance(double frame_seconds);

    /// The step about to run. Call once for each step `advance()` returned.
    [[nodiscard]] FixedStepTime next_step();

    /// The next `advance()` covers exactly one step, whatever the frame's
    /// time, and carries nothing: the editor's single step.
    void request_single_step() { single_step_ = true; }

    /// How far past the last step the frame sits, as a share of a step, in
    /// [0, 1).
    [[nodiscard]] double alpha() const;

    /// Simulation time at the end of the last step taken.
    [[nodiscard]] double time() const {
        return start_time_ + (static_cast<double>(index_) * dt_);
    }

    /// The simulation time rendering shows. What moved is drawn blended
    /// `alpha()` of the way through the last step, so anything drawn from a
    /// time, like the waves a hull floats on, has to be drawn at that same
    /// point in the step.
    [[nodiscard]] double render_time() const { return time() - dt_ + (alpha() * dt_); }

    [[nodiscard]] double step_seconds() const { return dt_; }
    [[nodiscard]] std::uint64_t index() const { return index_; }

    /// The most game time one frame may count for.
    [[nodiscard]] double max_frame_seconds() const { return MAX_STEPS_PER_FRAME * dt_; }

private:
    double dt_{1.0 / DEFAULT_TICK_RATE};
    double start_time_{0.0};
    double accumulator_{0.0};
    std::uint64_t index_{0};
    // Steps advance() handed out that next_step() has not taken yet.
    int pending_{0};
    bool single_step_{false};
};

} // namespace fjell
