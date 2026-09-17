#pragma once

#include "core/result.hpp"
#include "environment/environment_overrides.hpp"

#include <string>
#include <vector>

namespace fjell {

class EnvironmentSubsystem;
class ResourceManager;
class Scene;

/// A timed script of environment changes, for a capture that has to be the
/// same run twice.
///
/// Measuring what weather costs means comparing the same camera over the
/// same seconds with the weather doing the same thing, and driving that by
/// hand puts the change at a different second every time. A scenario states
/// the changes and their times, so two captures line up and a third with the
/// engine rebuilt is comparable to both.
///
/// The file is one step per line, a time in seconds then a command:
///
/// ```
/// # a storm arrives half a minute in, then clears
/// 0    hour 13
/// 0    day-length 0
/// 0    weather assets/environment/clear.fjweather sustain
/// 0    wetness 0
/// 30   weather assets/environment/rain.fjweather approach
/// 150  weather assets/environment/clear.fjweather onset
/// 180  quit
/// ```
///
/// Times are from the start of playback and need not be ordered; the loader
/// sorts them. Blank lines and `#` comments are ignored.
struct EnvironmentScenario {
    struct Step {
        /// Seconds from the start of the scenario.
        float time{0.0f};
        /// What to apply. A step carries the same optional fields as a
        /// launch override, so one command shape covers both.
        EnvironmentOverrides change;
        /// Close the engine at this step, which is what makes a capture run
        /// a fixed length rather than one someone has to stop.
        bool quit{false};
    };

    std::vector<Step> steps;

    /// How long the whole thing runs: the last step's time, or the quit.
    [[nodiscard]] float duration() const {
        return steps.empty() ? 0.0f : steps.back().time;
    }
};

/// Parse a scenario file. Reports the line a bad step is on rather than
/// skipping it, since a capture built on a misread script measures the
/// wrong thing without saying so.
[[nodiscard]] Result<EnvironmentScenario> load_environment_scenario(const std::string& path);

/// Plays a scenario against the environment: applies each step as its time
/// arrives, once. Advanced from the engine loop with the frame's delta.
class ScenarioPlayer {
public:
    void set_scenario(EnvironmentScenario scenario) {
        scenario_ = std::move(scenario);
        time_ = 0.0f;
        next_ = 0;
        quit_ = false;
    }

    [[nodiscard]] bool empty() const { return scenario_.steps.empty(); }

    /// Apply whatever is due. `environment` and `resources` are what the
    /// steps act on.
    void tick(float dt, Scene& scene, EnvironmentSubsystem& environment,
              ResourceManager& resources);

    /// The scenario reached a quit step, so the run is over.
    [[nodiscard]] bool finished() const { return quit_; }

    [[nodiscard]] float elapsed() const { return time_; }

private:
    EnvironmentScenario scenario_;
    float time_{0.0f};
    size_t next_{0};
    bool quit_{false};
};

} // namespace fjell
