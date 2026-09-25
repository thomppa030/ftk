#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace fjell {

/// How a play session's fixed steps are going, for the editor's stats panel:
/// what the last frame ran and what it cost, and how much game time the
/// catch-up limit has cut so far. PlayController resets it when play starts;
/// the engine's step loop fills it in every frame.
struct FixedStepStats {
    /// One subsystem's share of the last frame's steps.
    struct Part {
        std::string_view name;
        double seconds{0.0};
    };

    /// Steps per second of game time this session runs at; zero while no
    /// game is running.
    std::uint32_t tick_rate{0};

    /// How many steps the last frame ran.
    int steps{0};

    /// Wall time the last frame's steps took together, and the part of it
    /// each subsystem that runs in the step took, in the order they run.
    double seconds{0.0};
    std::vector<Part> parts;

    /// Game time the catch-up limit has cut from frames this session, and how
    /// many times the game began to run slow because of it.
    double dropped_seconds{0.0};
    int slowdowns{0};
};

} // namespace fjell
