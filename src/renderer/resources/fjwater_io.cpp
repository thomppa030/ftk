#include "renderer/resources/fjwater_io.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>

namespace fjell {

namespace {

[[nodiscard]] glm::vec2 read_vec2(const nlohmann::json& j, const char* key, glm::vec2 fallback) {
    if (!j.contains(key) || !j[key].is_array() || j[key].size() != 2) return fallback;
    return glm::vec2(j[key][0].get<float>(), j[key][1].get<float>());
}

[[nodiscard]] nlohmann::json write_vec2(glm::vec2 v) {
    return nlohmann::json::array({v.x, v.y});
}

} // namespace

std::vector<WaterWave> generate_water_waves(const WaterWaveGenerator& settings) {
    std::vector<WaterWave> waves;
    std::uint32_t count = std::min(settings.wave_count, MAX_WATER_WAVES);
    if (count == 0) return waves;

    glm::vec2 wind = settings.wind_direction;
    float wind_len = glm::length(wind);
    wind = (wind_len < 1e-6f) ? glm::vec2(1.0f, 0.0f) : wind / wind_len;

    float spread = glm::radians(settings.spread_degrees);

    for (std::uint32_t i = 0; i < count; ++i) {
        // Fan the trains across the wind, alternating sides so the sea has no
        // net drift away from the wind direction. The first train travels
        // straight downwind and carries the swell.
        // Trains come in pairs, one either side of the wind, each pair a step
        // further out: 0, +1, -1, +2, -2... Both divisions are meant to be
        // integer — they are counting pairs, not measuring an angle.
        //
        // pair_count is how many steps out the fan goes, which is the highest
        // pair_index any train reaches: count/2. Dividing by more than that
        // leaves the outermost train short of the spread it was promised — it
        // used to divide by count/2 + 1, so at six trains the fan reached
        // three quarters of the angle and a sea authored at 40 degrees never
        // opened past 30. Every train then sat near enough the wind that the
        // crests read as parallel ridges, and foam ran along one axis however
        // the shader composed it.
        std::uint32_t pair_index = (i + 1) / 2;
        std::uint32_t pair_count = std::max(count / 2, 1u);
        float side = (i % 2 == 0) ? 1.0f : -1.0f;
        float step = (count <= 1) ? 0.0f
                                  : static_cast<float>(pair_index) / static_cast<float>(pair_count);
        float angle = side * spread * step;

        float ca = std::cos(angle);
        float sa = std::sin(angle);

        WaterWave wave;
        wave.direction = glm::vec2(wind.x * ca - wind.y * sa, wind.x * sa + wind.y * ca);

        // Wavelengths halve down the list, so a few trains span a wide range
        // of scales: the long ones carry the swell, the short ones the detail.
        float scale = std::pow(std::clamp(settings.wavelength_falloff, 0.1f, 0.98f),
                               static_cast<float>(i));
        wave.wavelength = std::max(settings.longest_wavelength * scale, 0.5f);

        // Amplitude falls faster than wavelength, which is what keeps short
        // waves from turning the surface to noise. Steeper than linear so the
        // biggest train stays the one you read.
        wave.amplitude = settings.swell
                       * std::pow(std::clamp(settings.amplitude_falloff, 0.1f, 0.98f),
                                  static_cast<float>(i));

        // Shorter waves pinch harder: chop lives in the small stuff.
        wave.steepness = std::clamp(settings.chop * (0.6f + 0.4f * static_cast<float>(i)
                                                     / static_cast<float>(count)),
                                    0.0f, 1.0f);

        // Drift the speeds so the trains do not return to the same
        // arrangement on a short cycle and read as a repeating pattern.
        wave.speed = 1.0f + 0.08f * static_cast<float>(i);

        waves.push_back(wave);
    }
    return waves;
}

Result<FjWaterData> load_fjwater_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        return make_error("Failed to open .fjwater file: " + path);
    }

    nlohmann::json j;
    try {
        f >> j;
    } catch (const std::exception& e) {
        return make_error("Malformed .fjwater file " + path + ": " + e.what());
    }

    FjWaterData water;
    water.amplitude_scale = j.value("amplitude_scale", 1.0f);
    water.speed_scale = j.value("speed_scale", 1.0f);
    water.steepness_headroom = j.value("steepness_headroom", 1.10f);

    if (j.contains("generator") && j["generator"].is_object()) {
        const auto& g = j["generator"];
        water.generator.wind_direction = read_vec2(g, "wind_direction", glm::vec2(1.0f, 0.0f));
        water.generator.swell = g.value("swell", 1.0f);
        water.generator.chop = g.value("chop", 0.7f);
        water.generator.longest_wavelength = g.value("longest_wavelength", 30.0f);
        water.generator.wave_count = g.value("wave_count", 6u);
        water.generator.spread_degrees = g.value("spread_degrees", 40.0f);
        water.generator.wavelength_falloff = g.value("wavelength_falloff", 0.62f);
        water.generator.amplitude_falloff = g.value("amplitude_falloff", 0.68f);
    }

    if (j.contains("waves") && j["waves"].is_array()) {
        for (const auto& wj : j["waves"]) {
            if (water.waves.size() >= MAX_WATER_WAVES) break;

            WaterWave wave;
            wave.direction = read_vec2(wj, "direction", glm::vec2(1.0f, 0.0f));
            wave.wavelength = wj.value("wavelength", 20.0f);
            wave.amplitude = wj.value("amplitude", 0.5f);
            wave.steepness = wj.value("steepness", 0.5f);
            wave.speed = wj.value("speed", 1.0f);

            // A train that cannot move the surface is dropped rather than
            // kept as a row that quietly does nothing.
            if (glm::length(wave.direction) < 1e-6f || wave.wavelength <= 0.0f) continue;
            water.waves.push_back(wave);
        }
    }

    return water;
}

Result<> save_fjwater_file(const std::string& path, const FjWaterData& water) {
    nlohmann::json j;
    j["amplitude_scale"] = water.amplitude_scale;
    j["speed_scale"] = water.speed_scale;
    j["steepness_headroom"] = water.steepness_headroom;

    nlohmann::json g = nlohmann::json::object();
    g["wind_direction"] = write_vec2(water.generator.wind_direction);
    g["swell"] = water.generator.swell;
    g["chop"] = water.generator.chop;
    g["longest_wavelength"] = water.generator.longest_wavelength;
    g["wave_count"] = water.generator.wave_count;
    g["spread_degrees"] = water.generator.spread_degrees;
    g["wavelength_falloff"] = water.generator.wavelength_falloff;
    g["amplitude_falloff"] = water.generator.amplitude_falloff;
    j["generator"] = g;

    nlohmann::json waves = nlohmann::json::array();
    for (const auto& wave : water.waves) {
        nlohmann::json wj = nlohmann::json::object();
        wj["direction"] = write_vec2(wave.direction);
        wj["wavelength"] = wave.wavelength;
        wj["amplitude"] = wave.amplitude;
        wj["steepness"] = wave.steepness;
        wj["speed"] = wave.speed;
        waves.push_back(wj);
    }
    j["waves"] = waves;

    std::ofstream f(path);
    if (!f.is_open()) {
        return make_error("Failed to write .fjwater file: " + path);
    }
    f << j.dump(4) << "\n";
    return {};
}

} // namespace fjell
