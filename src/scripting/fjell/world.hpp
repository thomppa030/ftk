// world.hpp — Per-frame runtime state for game scripts
#pragma once

#include "fjell/input.hpp"

namespace fjell {

/// Per-frame runtime state shared by every script, reached through
/// `ScriptComponent::world()`. The convenience accessors on `ScriptComponent`
/// (`input()`, `delta_time()`, ...) forward to this, so reaching for `World`
/// directly is rarely necessary.
///
/// The engine refreshes it each frame; do not cache the values across frames.
struct World {
    /// Engine-populated input backing pointer. Use `input()` instead.
    const Input* input_{nullptr};

    /// Seconds since the previous frame, scaled by `Time::set_scale()` and 0
    /// while paused. Same value `tick(dt)` receives.
    float delta_time{0.0f};

    /// Seconds since play started, scaled and frozen while paused.
    float elapsed_time{0.0f};

    /// Keyboard and mouse state for this frame.
    [[nodiscard]] const Input& input() const { return *input_; }
};

} // namespace fjell
