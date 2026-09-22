#include "environment/day_profile_asset.hpp"

#include "environment/environment_state.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>

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
    // The fog's look is its material now; the two fog tracks fold into the
    // material's own uniforms by the names they always reached.
    if (const auto v = tracks.value_of(FOG_DENSITY, hour)) {
        if (auto* u = find_named(state.fog.uniforms, fog_track::DENSITY_UNIFORM)) {
            fold_track(*u, tracks.find(FOG_DENSITY)->mode, *v);
        }
    }
    if (const auto c = tracks.color_of(FOG_COLOR, hour)) {
        if (auto* u = find_named(state.fog.uniforms, fog_track::COLOR_UNIFORM)) {
            fold_track(*u, tracks.find(FOG_COLOR)->mode, *c);
        }
    }
    tracks.apply(EXPOSURE_BIAS, hour, state.exposure_bias);
    tracks.apply(STAR_INTENSITY, hour, state.star_intensity);

    // Grouped fog tracks reach the scene's fog volumes: `fog_density.valley`
    // scales every volume grouped `valley`, leaving the global fog and every
    // other group alone. The hour gives a mist its shape; a weather event
    // scales what the hour decided, later and on top.
    for (const auto& track : tracks.tracks) {
        const auto density_group =
            fog_group_track::group_of(track.name, fog_group_track::DENSITY_PREFIX);
        if (!density_group.empty()) {
            if (const auto v = tracks.value_of(track.name, hour)) {
                for (auto& volume : state.fog_volumes) {
                    if (volume.group != density_group) continue;
                    if (auto* u = find_named(volume.uniforms, fog_track::DENSITY_UNIFORM)) {
                        fold_track(*u, track.mode, *v);
                    }
                }
            }
            continue;
        }
        const auto color_group =
            fog_group_track::group_of(track.name, fog_group_track::COLOR_PREFIX);
        if (!color_group.empty()) {
            if (const auto c = tracks.color_of(track.name, hour)) {
                for (auto& volume : state.fog_volumes) {
                    if (volume.group != color_group) continue;
                    if (auto* u = find_named(volume.uniforms, fog_track::COLOR_UNIFORM)) {
                        fold_track(*u, track.mode, *c);
                    }
                }
            }
        }
    }

    // Anything else is a sky uniform. A later value of the same name, from a
    // profile or a node override applied before this, is replaced: the
    // profile is the animated source and wins over a static override.
    for (const auto& track : tracks.tracks) {
        if (is_builtin(track.name)) continue;
        if (!fog_group_track::group_of(track.name, fog_group_track::DENSITY_PREFIX).empty() ||
            !fog_group_track::group_of(track.name, fog_group_track::COLOR_PREFIX).empty()) {
            continue;
        }
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

Result<DayProfileAsset> load_day_profile_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        return make_error("Failed to open .fjday file: " + path);
    }
    nlohmann::json j;
    try {
        f >> j;
    } catch (const std::exception& e) {
        return make_error("Malformed .fjday file " + path + ": " + e.what());
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

Result<> save_day_profile_file(const std::string& path, const DayProfileAsset& asset) {
    nlohmann::json j;
    j["name"] = asset.name;
    asset.tracks.to_json(j["tracks"]);
    std::ofstream f(path);
    if (!f.is_open()) {
        return make_error("Failed to write .fjday file: " + path);
    }
    f << j.dump(4) << "\n";
    return {};
}

} // namespace fjell
