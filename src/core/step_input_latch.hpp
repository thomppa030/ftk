#pragma once

#include <string>
#include <unordered_set>

namespace fjell {

/// The button edges a fixed step sees. Frames and steps do not line up: a
/// frame may run no step or several. A press on a frame that runs no step must
/// still reach the next step, and one on a frame that runs two must reach only
/// the first of them. So each frame's edges are recorded as they happen and
/// handed to the next step that begins.
///
/// Held buttons and axes need none of this: every step of a frame sees them
/// as that frame left them.
class StepInputLatch {
public:
    /// Note one action's edges for this frame. Called for every action after
    /// each input update while the game runs.
    void record(const std::string& action, bool pressed, bool released);

    /// A step begins: its edges are everything recorded since the last step
    /// began.
    void begin_step();

    /// Forget everything, for a new session.
    void reset();

    /// Whether the action was pressed since the step before this one.
    [[nodiscard]] bool pressed(const std::string& action) const;
    /// Whether the action was released since the step before this one.
    [[nodiscard]] bool released(const std::string& action) const;

private:
    // Recorded since the current step began, waiting for the next one.
    std::unordered_set<std::string> pending_pressed_;
    std::unordered_set<std::string> pending_released_;
    // The current step's.
    std::unordered_set<std::string> pressed_;
    std::unordered_set<std::string> released_;
};

} // namespace fjell
