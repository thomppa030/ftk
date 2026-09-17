#pragma once

#include "environment/weather_asset.hpp"

#include <optional>
#include <string>

namespace fjell {

class EnvironmentSubsystem;
class ResourceManager;
class Scene;

/// What a launch asks of the environment, on top of what the scene authors.
///
/// A perf capture wants one scene under many conditions — noon and midnight,
/// clear and mid-storm — and editing the scene between runs makes each
/// capture a different scene. These override the scene for one run without
/// touching the file, so what a run measured is what its command line says.
///
/// Every field is optional and an unset one leaves the scene's own value
/// alone.
struct EnvironmentOverrides {
    /// The hour to start at, 0..24.
    std::optional<float> hour;
    /// Real seconds for a full day. 0 freezes the clock, which is what a
    /// capture wants: a moving sun re-lights the scene and re-traces the GI
    /// under the measurement.
    std::optional<float> day_length;
    /// A .fjweather to play instead of the scene's, project-relative.
    std::optional<std::string> weather;
    /// Which phase that event opens in.
    std::optional<WeatherPhase> weather_phase;
    /// Freeze the climate so nothing new is rolled mid-capture.
    std::optional<bool> hold_weather;
    /// The seed every weather roll derives from. Fixing it makes two runs
    /// roll the same phase durations, so a capture differs by the change
    /// under test rather than by the weather having been a bit different.
    std::optional<uint32_t> seed;
    /// How wet and how snow-covered surfaces already are, 0..1. These
    /// integrate from zero over minutes, so starting mid-storm otherwise
    /// starts on dry ground; setting them puts the world where the weather
    /// would have left it. They keep integrating afterwards.
    std::optional<float> wetness;
    std::optional<float> snow;

    [[nodiscard]] bool any() const {
        return hour || day_length || weather || weather_phase || hold_weather || seed
            || wetness || snow;
    }
};

/// Apply overrides to a loaded scene. Most of them are written onto the
/// World Environment node rather than handed to the subsystem, because the
/// subsystem rebinds from that node every tick and would overwrite anything
/// set beside it. Call once the scene is loaded; the next tick is what
/// starts the weather.
void apply_environment_overrides(const EnvironmentOverrides& overrides,
                                 Scene& scene,
                                 EnvironmentSubsystem& environment,
                                 ResourceManager& resources);

} // namespace fjell
