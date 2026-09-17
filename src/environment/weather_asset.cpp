#include "environment/weather_asset.hpp"

#include "core/math/color_space.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace fjell {

namespace weather_track {

const std::vector<std::string_view>& all() {
    static const std::vector<std::string_view> names = {
        CLOUD_COVERAGE, CLOUD_DENSITY, PRECIPITATION,
        WIND_SPEED, WIND_GUST, WIND_VEER,
        FOG_DENSITY, FOG_COLOR, SUN_INTENSITY, AMBIENT_INTENSITY, SKY_TINT,
        LIGHTNING_RATE, SKY_BLEND,
    };
    return names;
}

TrackKind kind_of(std::string_view name) {
    return name == FOG_COLOR || name == SKY_TINT ? TrackKind::Color : TrackKind::Float;
}

bool is_builtin(std::string_view name) {
    const auto& names = all();
    return std::find(names.begin(), names.end(), name) != names.end();
}

} // namespace weather_track

namespace {

const char* precipitation_name(PrecipitationType type) {
    switch (type) {
        case PrecipitationType::Rain: return "rain";
        case PrecipitationType::Snow: return "snow";
        default: return "none";
    }
}

PrecipitationType precipitation_from(const std::string& name) {
    if (name == "rain") return PrecipitationType::Rain;
    if (name == "snow") return PrecipitationType::Snow;
    return PrecipitationType::None;
}

nlohmann::json phase_to_json(const PhaseDuration& d) {
    nlohmann::json j;
    j["min"] = d.min_seconds;
    j["max"] = d.max_seconds;
    if (d.indefinite) j["indefinite"] = true;
    return j;
}

PhaseDuration phase_from_json(const nlohmann::json& j, const PhaseDuration& fallback) {
    PhaseDuration d = fallback;
    if (!j.is_object()) return d;
    d.min_seconds = std::max(j.value("min", d.min_seconds), 0.0f);
    d.max_seconds = std::max(j.value("max", d.max_seconds), d.min_seconds);
    d.indefinite = j.value("indefinite", d.indefinite);
    return d;
}

} // namespace

const char* weather_phase_name(WeatherPhase phase) {
    switch (phase) {
        case WeatherPhase::Approach: return "approach";
        case WeatherPhase::Onset: return "onset";
        case WeatherPhase::Sustain: return "sustain";
        case WeatherPhase::Tail: return "tail";
    }
    return "sustain";
}

std::optional<WeatherPhase> weather_phase_from_name(std::string_view name) {
    for (uint32_t i = 0; i < WEATHER_PHASE_COUNT; ++i) {
        const auto phase = static_cast<WeatherPhase>(i);
        if (name == weather_phase_name(phase)) return phase;
    }
    return std::nullopt;
}

Result<WeatherAsset> load_weather_asset_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        return make_error("Failed to open .fjweather file: " + path);
    }
    nlohmann::json j;
    try {
        f >> j;
    } catch (const std::exception& e) {
        return make_error("Malformed .fjweather file " + path + ": " + e.what());
    }

    WeatherAsset asset;
    asset.name = j.value("name", std::string{});
    if (asset.name.empty()) {
        asset.name = std::filesystem::path(path).stem().string();
    }
    if (j.contains("phases") && j["phases"].is_object()) {
        const auto& jp = j["phases"];
        for (uint32_t i = 0; i < WEATHER_PHASE_COUNT; ++i) {
            const char* key = weather_phase_name(static_cast<WeatherPhase>(i));
            if (jp.contains(key)) asset.phases[i] = phase_from_json(jp[key], asset.phases[i]);
        }
    }
    asset.precipitation_type = precipitation_from(j.value("precipitation", std::string{"none"}));
    asset.wetness_target = std::clamp(j.value("wetness_target", 1.0f), 0.0f, 1.0f);
    asset.snow_target = std::clamp(j.value("snow_target", 1.0f), 0.0f, 1.0f);
    asset.wind_heading = j.value("wind_heading", 0.0f);
    if (j.contains("lightning") && j["lightning"].is_object()) {
        const auto& jl = j["lightning"];
        auto& l = asset.lightning;
        l.duration = std::max(jl.value("duration", l.duration), 0.01f);
        if (jl.contains("color")) {
            const auto c = color_space::srgb_to_linear(
                glm::vec3{jl["color"][0].get<float>(), jl["color"][1].get<float>(),
                          jl["color"][2].get<float>()});
            l.color = c;
        }
        l.intensity = jl.value("intensity", l.intensity);
        l.radius = jl.value("radius", l.radius);
        l.min_distance = jl.value("min_distance", l.min_distance);
        l.max_distance = std::max(jl.value("max_distance", l.max_distance), l.min_distance);
        l.vfx = jl.value("vfx", std::string{});
        if (jl.contains("flash") && jl["flash"].is_array()) {
            for (const auto& k : jl["flash"]) {
                CurveKeyframe key;
                key.time = std::clamp(k.value("t", 0.0f), 0.0f, 1.0f);
                key.value = k.value("v", 0.0f);
                key.tangent_in = k.value("in", 0.0f);
                key.tangent_out = k.value("out", 0.0f);
                l.flash.keyframes.push_back(key);
            }
            std::stable_sort(l.flash.keyframes.begin(), l.flash.keyframes.end(),
                             [](const CurveKeyframe& a, const CurveKeyframe& b) {
                                 return a.time < b.time;
                             });
        }
    }
    asset.sky_shader = j.value("sky_shader", std::string{});
    asset.day_profile = j.value("day_profile", std::string{});
    if (j.contains("vfx") && j["vfx"].is_object()) {
        const auto& jv = j["vfx"];
        asset.vfx.rain = jv.value("rain", std::string{});
        asset.vfx.snow = jv.value("snow", std::string{});
        asset.vfx.splash = jv.value("splash", std::string{});
    }
    if (j.contains("audio") && j["audio"].is_object()) {
        const auto& ja = j["audio"];
        asset.audio.bed = ja.value("bed", std::string{});
        asset.audio.wind = ja.value("wind", std::string{});
        asset.audio.thunder = ja.value("thunder", std::string{});
    }
    if (j.contains("tracks")) {
        asset.tracks.from_json(j["tracks"]);
    }
    // The domain is the four phases; the file cannot say otherwise.
    asset.tracks.length = static_cast<float>(WEATHER_PHASE_COUNT);
    asset.tracks.wrap = false;
    return asset;
}

Result<> save_weather_asset_file(const std::string& path, const WeatherAsset& asset) {
    nlohmann::json j;
    j["name"] = asset.name;
    auto& jp = j["phases"] = nlohmann::json::object();
    for (uint32_t i = 0; i < WEATHER_PHASE_COUNT; ++i) {
        jp[weather_phase_name(static_cast<WeatherPhase>(i))] = phase_to_json(asset.phases[i]);
    }
    j["precipitation"] = precipitation_name(asset.precipitation_type);
    j["wetness_target"] = asset.wetness_target;
    j["snow_target"] = asset.snow_target;
    j["wind_heading"] = asset.wind_heading;
    {
        const auto& l = asset.lightning;
        nlohmann::json jl;
        jl["duration"] = l.duration;
        const glm::vec3 srgb = color_space::linear_to_srgb(l.color);
        jl["color"] = {srgb.r, srgb.g, srgb.b};
        jl["intensity"] = l.intensity;
        jl["radius"] = l.radius;
        jl["min_distance"] = l.min_distance;
        jl["max_distance"] = l.max_distance;
        if (!l.vfx.empty()) jl["vfx"] = l.vfx;
        if (!l.flash.keyframes.empty()) {
            auto& keys = jl["flash"] = nlohmann::json::array();
            for (const auto& k : l.flash.keyframes) {
                keys.push_back({{"t", k.time}, {"v", k.value},
                                {"in", k.tangent_in}, {"out", k.tangent_out}});
            }
        }
        j["lightning"] = std::move(jl);
    }
    if (!asset.sky_shader.empty()) j["sky_shader"] = asset.sky_shader;
    if (!asset.day_profile.empty()) j["day_profile"] = asset.day_profile;
    nlohmann::json jv = nlohmann::json::object();
    if (!asset.vfx.rain.empty()) jv["rain"] = asset.vfx.rain;
    if (!asset.vfx.snow.empty()) jv["snow"] = asset.vfx.snow;
    if (!asset.vfx.splash.empty()) jv["splash"] = asset.vfx.splash;
    if (!jv.empty()) j["vfx"] = std::move(jv);
    nlohmann::json ja = nlohmann::json::object();
    if (!asset.audio.bed.empty()) ja["bed"] = asset.audio.bed;
    if (!asset.audio.wind.empty()) ja["wind"] = asset.audio.wind;
    if (!asset.audio.thunder.empty()) ja["thunder"] = asset.audio.thunder;
    if (!ja.empty()) j["audio"] = std::move(ja);
    asset.tracks.to_json(j["tracks"]);

    std::ofstream f(path);
    if (!f.is_open()) {
        return make_error("Failed to write .fjweather file: " + path);
    }
    f << j.dump(4) << "\n";
    return {};
}

} // namespace fjell
