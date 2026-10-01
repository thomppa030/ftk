#include "environment/day_profile_asset.hpp"

#include "environment/environment_state.hpp"
#include "environment/material_track.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fjell {

namespace day_track {

const std::vector<std::string_view>& all() {
    static const std::vector<std::string_view> names = {
        SUN_COLOR, SUN_INTENSITY, MOON_COLOR, MOON_INTENSITY,
        AMBIENT_COLOR, AMBIENT_INTENSITY, SKY_TOP, SKY_BOTTOM,
        FOG_COLOR, FOG_DENSITY, EXPOSURE_BIAS, STAR_INTENSITY,
    };
    return names;
}

TrackKind kind_of(std::string_view name) {
    if (name == SUN_COLOR || name == MOON_COLOR || name == AMBIENT_COLOR ||
        name == SKY_TOP || name == SKY_BOTTOM || name == FOG_COLOR) {
        return TrackKind::Color;
    }
    return TrackKind::Float;
}

bool is_builtin(std::string_view name) {
    const auto& names = all();
    return std::find(names.begin(), names.end(), name) != names.end();
}

} // namespace day_track

void DayProfileAsset::apply(float hour, EnvironmentState& state) const {
    using namespace day_track;
    tracks.apply(SUN_COLOR, hour, state.sun.color);
    tracks.apply(SUN_INTENSITY, hour, state.sun.intensity);
    tracks.apply(MOON_COLOR, hour, state.moon.color);
    tracks.apply(MOON_INTENSITY, hour, state.moon.intensity);
    tracks.apply(AMBIENT_COLOR, hour, state.ambient_color);
    tracks.apply(AMBIENT_INTENSITY, hour, state.ambient_intensity);
    tracks.apply(SKY_TOP, hour, state.sky.top_color);
    tracks.apply(SKY_BOTTOM, hour, state.sky.bottom_color);
    tracks.apply(EXPOSURE_BIAS, hour, state.exposure_bias);
    tracks.apply(STAR_INTENSITY, hour, state.star_intensity);

    // Material tracks name a uniform of a fog or post volume's material:
    // `fog.density` thickens the haze, `fog.valley.noise_contrast` breaks the
    // valley's banks into wisps, `post.saturation` drains the colour at dusk.
    // The hour gives a mist its shape; a weather event scales what the hour
    // decided, later and on top.
    for (const auto& track : tracks.tracks) {
        const auto target = material_track::parse(track.name);
        if (!target) continue;
        if (track.kind == TrackKind::Color) {
            if (const auto c = tracks.color_of(track.name, hour)) {
                material_track::apply(state, *target, track.mode, *c);
            }
        } else if (const auto v = tracks.value_of(track.name, hour)) {
            material_track::apply(state, *target, track.mode, *v);
        }
    }

    // Anything else is a sky uniform. A later value of the same name, from a
    // profile or a node override applied before this, is replaced: the
    // profile is the animated source and wins over a static override.
    for (const auto& track : tracks.tracks) {
        if (is_builtin(track.name) || material_track::parse(track.name)) continue;
        NamedValue value;
        value.name = track.name;
        if (track.kind == TrackKind::Color) {
            const auto c = tracks.color_of(track.name, hour);
            if (!c) continue;
            value.count = 3;
            value.values = {c->r, c->g, c->b, 0.0f};
        } else {
            const auto v = tracks.value_of(track.name, hour);
            if (!v) continue;
            value.count = 1;
            value.values = {*v, 0.0f, 0.0f, 0.0f};
        }
        auto it = std::find_if(state.sky_uniforms.begin(), state.sky_uniforms.end(),
                               [&](const NamedValue& n) { return n.name == value.name; });
        if (it != state.sky_uniforms.end()) {
            *it = std::move(value);
        } else {
            state.sky_uniforms.push_back(std::move(value));
        }
    }
}

ftk::Result<DayProfileAsset> load_day_profile_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        return ftk::make_error("Failed to open .fjday file: " + path);
    }
    std::stringstream text;
    text << f.rdbuf();
    return day_profile_from_text(text.str(), path);
}

ftk::Result<DayProfileAsset> day_profile_from_text(std::string_view text, const std::string& path) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(text);
    } catch (const std::exception& e) {
        return ftk::make_error("Malformed .fjday file " + path + ": " + e.what());
    }
    DayProfileAsset asset;
    asset.name = j.value("name", std::string{});
    if (asset.name.empty()) {
        asset.name = std::filesystem::path(path).stem().string();
    }
    if (j.contains("tracks")) {
        asset.tracks.from_json(j["tracks"]);
    }
    // A day is always 24 hours around; the file cannot say otherwise.
    asset.tracks.length = 24.0f;
    asset.tracks.wrap = true;
    return asset;
}

std::string day_profile_text(const DayProfileAsset& asset) {
    nlohmann::json j;
    j["name"] = asset.name;
    asset.tracks.to_json(j["tracks"]);
    return j.dump(4) + "\n";
}

ftk::Result<> save_day_profile_file(const std::string& path, const DayProfileAsset& asset) {
    std::ofstream f(path);
    if (!f.is_open()) {
        return ftk::make_error("Failed to write .fjday file: " + path);
    }
    f << day_profile_text(asset);
    return {};
}

} // namespace fjell
