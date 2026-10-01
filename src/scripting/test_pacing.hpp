#pragma once

#include "ftk/base/result.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fjell {

class PhysicsWorld;
class Scene;

// `--run-tests --pacing 30,144,jitter` plays every functional test once per
// pacing, with the engine's clock told each frame took what the pacing says
// rather than what it did. The simulation steps the same whatever the frames
// are, so each run must leave the same state after every step; the first step
// where a run differs from the first pacing's is a frame rate leaking into
// the simulation, such as a script integrating with the frame's dt in tick().

/// A frame rate a paced run plays at: frames all of one length, or uneven
/// ones.
struct Pacing {
    /// As the report names it: "30 fps", "jitter".
    std::string name;
    /// Frames per second, or 0 for uneven frames.
    double fps{0.0};
};

/// Reads `--pacing`'s list: frame rates and `jitter`, separated by commas.
/// At least two, because each run is compared with the first.
[[nodiscard]] ftk::Result<std::vector<Pacing>> parse_pacings(std::string_view list);

/// The frames a paced run plays, as the clock is told they took. Uneven
/// frames come from a fixed seed, so every run of `jitter` plays the same ones.
class PacedFrames {
public:
    explicit PacedFrames(const Pacing& pacing);

    /// The next frame's length in seconds.
    [[nodiscard]] double next();

private:
    double fps_{0.0};
    std::mt19937 rng_;
    std::uniform_real_distribution<double> uneven_;
    int frames_{0};
};

/// Where two paced runs of one test part: the step, and the first node and
/// value that differ there.
struct Divergence {
    std::size_t step{0};
    std::string node;
    std::string field;
    std::string first;   ///< The value in the run compared against.
    std::string second;  ///< The value in the other run.
};

/// The simulation as one paced run left it after every step.
class StepRecording {
public:
    /// Keep what the simulation holds after a step: every body and
    /// character, with its motion, and every other node moved in the step.
    /// A node moved outside the steps, like a camera rig in late_tick(), is
    /// drawn rather than simulated, and is left out.
    void record(Scene& scene, const PhysicsWorld* physics);

    [[nodiscard]] std::size_t steps() const { return steps_.size(); }

    /// The first step, node and value at which `other` differs from this
    /// recording, comparing the steps both have. Nothing when they agree to
    /// the bit.
    [[nodiscard]] std::optional<Divergence> first_divergence(const StepRecording& other) const;

private:
    // Position, rotation, scale, velocity, spin and ground state, in the
    // order FIELD_NAMES in the source names them.
    static constexpr std::size_t VALUE_COUNT = 17;
    struct NodeState {
        uint32_t id{0};
        std::array<float, VALUE_COUNT> values{};
    };
    using Step = std::vector<NodeState>;

    [[nodiscard]] std::string name_of(uint32_t id) const;

    std::vector<Step> steps_;
    std::unordered_map<uint32_t, std::string> names_;
};

} // namespace fjell
