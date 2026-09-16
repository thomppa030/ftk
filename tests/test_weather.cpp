#include "environment/environment_state.hpp"
#include "environment/weather_asset.hpp"
#include "environment/weather_player.hpp"
#include "environment/cloud_shadow_frame.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>

#include <cmath>
#include <filesystem>

using namespace fjell;
using Catch::Approx;

namespace {

// A rain event with fixed phase lengths so the test knows where it is.
WeatherAsset make_rain() {
    WeatherAsset rain;
    rain.name = "rain";
    rain.precipitation_type = PrecipitationType::Rain;
    rain.wetness_target = 0.8f;
    rain.wind_heading = 90.0f;   // blows toward +X
    rain.phases = {PhaseDuration{10.0f, 10.0f, false}, PhaseDuration{5.0f, 5.0f, false},
                   PhaseDuration{20.0f, 20.0f, false}, PhaseDuration{10.0f, 10.0f, false}};
    // Precipitation: nothing in the approach, up over the onset, full in the
    // sustain, down over the tail. Keys in phase units.
    rain.tracks.add(std::string(weather_track::PRECIPITATION), TrackKind::Float, TrackMode::Override)
        .curve.keyframes = {{1.0f / 4.0f, 0.0f, 0.0f, 0.0f}, {2.0f / 4.0f, 1.0f, 0.0f, 0.0f},
                            {3.0f / 4.0f, 1.0f, 0.0f, 0.0f}, {4.0f / 4.0f, 0.0f, 0.0f, 0.0f}};
    rain.tracks.add(std::string(weather_track::CLOUD_COVERAGE), TrackKind::Float, TrackMode::Override)
        .curve.keyframes = {{0.0f, 0.0f, 0.0f, 0.0f}, {1.0f / 4.0f, 0.9f, 0.0f, 0.0f},
                            {3.0f / 4.0f, 0.9f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 0.0f}};
    rain.tracks.add(std::string(weather_track::SUN_INTENSITY), TrackKind::Float, TrackMode::Multiply)
        .curve.keyframes = {{0.0f, 1.0f, 0.0f, 0.0f}, {1.0f / 4.0f, 0.3f, 0.0f, 0.0f},
                            {3.0f / 4.0f, 0.3f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f, 0.0f}};
    rain.tracks.add(std::string(weather_track::WIND_SPEED), TrackKind::Float, TrackMode::Override)
        .curve.keyframes = {{2.0f / 4.0f, 8.0f, 0.0f, 0.0f}};
    rain.tracks.add("temperature", TrackKind::Float, TrackMode::Override)
        .curve.keyframes = {{0.5f, 4.0f, 0.0f, 0.0f}};
    return rain;
}

} // namespace

TEST_CASE("A weather event walks its phases on rolled durations", "[weather]") {
    WeatherAsset rain = make_rain();
    WeatherPlayer player;
    player.play(&rain, 7, 1);
    REQUIRE(player.active() != nullptr);
    REQUIRE(player.active()->phase == WeatherPhase::Approach);
    REQUIRE(player.active()->durations[0] == Approx(10.0f));

    player.tick(5.0f);
    REQUIRE(player.active()->phase == WeatherPhase::Approach);
    REQUIRE(player.active()->progress() == Approx(0.5f));
    REQUIRE(player.active()->phase_units() == Approx(0.5f));
    REQUIRE(player.active()->weight() == Approx(0.25f));

    player.tick(7.0f);   // 12 s: 2 s into the onset
    REQUIRE(player.active()->phase == WeatherPhase::Onset);
    REQUIRE(player.active()->progress() == Approx(0.4f));

    player.tick(13.0f);  // 25 s: 10 s into the sustain
    REQUIRE(player.active()->phase == WeatherPhase::Sustain);
    REQUIRE(player.active()->weight() == Approx(1.0f));

    player.tick(15.0f);  // 40 s: 5 s into the tail
    REQUIRE(player.active()->phase == WeatherPhase::Tail);
    REQUIRE(player.active()->weight() == Approx(0.5f));

    player.tick(6.0f);   // done
    REQUIRE(player.active() == nullptr);
    REQUIRE(player.outgoing() == nullptr);
}

TEST_CASE("An indefinite sustain holds until the next event", "[weather]") {
    WeatherAsset clear;
    clear.phases[2].indefinite = true;
    clear.phases[0] = PhaseDuration{1.0f, 1.0f, false};
    clear.phases[1] = PhaseDuration{1.0f, 1.0f, false};
    WeatherPlayer player;
    player.play(&clear, 1, 3);
    player.tick(1000.0f);
    REQUIRE(player.active() != nullptr);
    REQUIRE(player.active()->phase == WeatherPhase::Sustain);
    REQUIRE(player.active()->progress() == Approx(1.0f));

    // A new event moves the old one into its tail underneath.
    WeatherAsset rain = make_rain();
    player.play(&rain, 2, 4);
    REQUIRE(player.active()->event_id == 2);
    REQUIRE(player.outgoing() != nullptr);
    REQUIRE(player.outgoing()->phase == WeatherPhase::Tail);
    player.tick(200.0f);
    REQUIRE(player.outgoing() == nullptr);
}

TEST_CASE("Compositing shapes the environment and integrates wetness", "[weather]") {
    WeatherAsset rain = make_rain();
    WeatherPlayer player;
    WeatherRates rates{.wetting_seconds = 10.0f, .drying_seconds = 100.0f,
                       .snowing_seconds = 10.0f, .melting_seconds = 100.0f};

    EnvironmentState state;
    state.sun.intensity = 2.0f;
    player.play(&rain, 7, 1);

    // Mid-approach: clouds building from windward, no rain yet, sun dimming.
    player.tick(5.0f);
    player.composite(state, 0.0f, rates);
    REQUIRE(state.weather.event_id == 7);
    REQUIRE(state.weather.phase == WeatherPhase::Approach);
    REQUIRE(state.weather.precipitation == Approx(0.0f));
    REQUIRE(state.weather.cloud_coverage > 0.0f);
    REQUIRE(state.weather.cloud_coverage < 0.9f);
    REQUIRE(state.sun.intensity < 2.0f);
    REQUIRE(state.sun.intensity > 0.6f);
    REQUIRE(state.weather.wetness == Approx(0.0f));
    // The front travels with the wind, toward +X, and its edge is sweeping.
    REQUIRE(state.weather.front_direction.x == Approx(1.0f).margin(1e-4));
    REQUIRE(state.weather.front_edge > -1.0f);
    REQUIRE(state.weather.front_edge < 1.5f);
    REQUIRE(state.weather.params.size() == 1);
    REQUIRE(state.weather.params[0].name == "temperature");
    REQUIRE(state.weather.params[0].values[0] == Approx(4.0f));

    // Into the sustain: full rain, surfaces wetting toward the target. Each
    // frame composites onto a freshly resolved state, as the subsystem does.
    player.tick(20.0f);   // 25 s
    for (int i = 0; i < 50; ++i) {
        state = EnvironmentState{};
        state.sun.intensity = 2.0f;
        player.composite(state, 1.0f, rates);
    }
    REQUIRE(state.weather.phase == WeatherPhase::Sustain);
    REQUIRE(state.weather.precipitation == Approx(1.0f));
    REQUIRE(state.weather.precipitation_type == PrecipitationType::Rain);
    REQUIRE(state.weather.wetness == Approx(0.8f).margin(0.02f));
    REQUIRE(state.weather.snow == Approx(0.0f));
    REQUIRE(state.weather.wind_speed == Approx(8.0f));
    // The drift is the wind integrated: fifty seconds at 8 m/s toward +X,
    // give or take the gusts, and always forward.
    REQUIRE(state.weather.wind_drift.x > 200.0f);
    REQUIRE(state.weather.wind_drift.x < 600.0f);
    REQUIRE(std::abs(state.weather.wind_drift.y) < 1e-3f);
    REQUIRE(state.weather.front_edge < -1.0f);        // covered everywhere
    REQUIRE(state.sun.intensity == Approx(0.6f).margin(1e-3));

    // After the event: dry weather, wetness decays slowly rather than snapping.
    player.tick(100.0f);
    REQUIRE(player.active() == nullptr);
    state = EnvironmentState{};
    player.composite(state, 10.0f, rates);
    REQUIRE(state.weather.event_id == 0);
    REQUIRE(state.weather.precipitation == Approx(0.0f));
    REQUIRE(state.weather.wetness > 0.6f);
    REQUIRE(state.weather.wetness < 0.8f);
    for (int i = 0; i < 100; ++i) {
        state = EnvironmentState{};
        player.composite(state, 10.0f, rates);
    }
    REQUIRE(state.weather.wetness < 0.01f);
}

TEST_CASE("Weather assets round-trip through their file", "[weather]") {
    WeatherAsset rain = make_rain();
    rain.sky_shader = "shaders/storm_sky.fjsl";
    rain.vfx.rain = "assets/vfx/rain.fjvfx";
    rain.audio.bed = "assets/sound/rain.fjaudio";
    rain.phases[2].indefinite = true;
    const std::string path = (std::filesystem::temp_directory_path() / "fjell_test_rain.fjweather").string();
    REQUIRE(save_weather_asset_file(path, rain));
    auto back = load_weather_asset_file(path);
    REQUIRE(back);
    REQUIRE(back->name == "rain");
    REQUIRE(back->precipitation_type == PrecipitationType::Rain);
    REQUIRE(back->wetness_target == Approx(0.8f));
    REQUIRE(back->wind_heading == Approx(90.0f));
    REQUIRE(back->sky_shader == "shaders/storm_sky.fjsl");
    REQUIRE(back->vfx.rain == "assets/vfx/rain.fjvfx");
    REQUIRE(back->audio.bed == "assets/sound/rain.fjaudio");
    REQUIRE(back->phases[2].indefinite);
    REQUIRE(back->phases[0].min_seconds == Approx(10.0f));
    REQUIRE(back->tracks.length == Approx(4.0f));
    REQUIRE_FALSE(back->tracks.wrap);
    REQUIRE(*back->tracks.value_of(weather_track::PRECIPITATION, 2.5f) == Approx(1.0f));
    REQUIRE(back->tracks.find("temperature") != nullptr);
    REQUIRE(weather_track::kind_of(weather_track::SKY_TINT) == TrackKind::Color);
    REQUIRE_FALSE(weather_track::is_builtin("temperature"));
}

TEST_CASE("The cloud shadow frame follows the camera in whole texels", "[weather]") {
    const float size = 512.0f;
    const uint32_t res = 256;   // two metres per texel
    auto a = cloud_shadow_frame(glm::vec3(100.0f, 5.0f, -40.0f), size, res);
    REQUIRE(a.size == Approx(size));
    // The camera sits inside the square, near its middle.
    REQUIRE(a.min_xz.x <= 100.0f - size * 0.5f + 2.0f);
    REQUIRE(a.min_xz.x >= 100.0f - size * 0.5f - 2.0f);
    REQUIRE(a.min_xz.y <= -40.0f - size * 0.5f + 2.0f);
    // Corners are on the texel grid.
    REQUIRE(std::fmod(a.min_xz.x, 2.0f) == Approx(0.0f));
    REQUIRE(std::fmod(a.min_xz.y, 2.0f) == Approx(0.0f));
    // A sub-texel camera move does not move the map; a full texel does.
    auto b = cloud_shadow_frame(glm::vec3(100.9f, 5.0f, -40.0f), size, res);
    REQUIRE(b.min_xz == a.min_xz);
    auto c = cloud_shadow_frame(glm::vec3(102.5f, 5.0f, -40.0f), size, res);
    REQUIRE(c.min_xz.x == Approx(a.min_xz.x + 2.0f));
    // The matrix maps the square's corners to uv 0 and 1.
    glm::vec4 lo = a.world_to_uv * glm::vec4(a.min_xz.x, 0.0f, a.min_xz.y, 1.0f);
    glm::vec4 hi = a.world_to_uv * glm::vec4(a.min_xz.x + size, 30.0f, a.min_xz.y + size, 1.0f);
    REQUIRE(lo.x == Approx(0.0f));
    REQUIRE(lo.y == Approx(0.0f));
    REQUIRE(hi.x == Approx(1.0f));
    REQUIRE(hi.y == Approx(1.0f));
}
