#include "environment/climate_asset.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

namespace fjell {

namespace {

float wrap_24(float hour) {
    float h = std::fmod(hour, 24.0f);
    return h < 0.0f ? h + 24.0f : h;
}

} // namespace

bool ClimateState::allows_hour(float hour) const {
    if (from_hour == to_hour) return true;   // any hour
    const float h = wrap_24(hour);
    const float from = wrap_24(from_hour);
    const float to = wrap_24(to_hour);
    // A window that runs past midnight is two ranges either side of it.
    if (from <= to) return h >= from && h < to;
    return h >= from || h < to;
}

const ClimateState* ClimateAsset::find(std::string_view state_name) const {
    const int i = index_of(state_name);
    return i >= 0 ? &states[static_cast<size_t>(i)] : nullptr;
}

int ClimateAsset::index_of(std::string_view state_name) const {
    for (size_t i = 0; i < states.size(); ++i) {
        if (states[i].name == state_name) return static_cast<int>(i);
    }
    return -1;
}

Result<ClimateAsset> load_climate_asset_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        return make_error("Failed to open .fjclimate file: " + path);
    }
    nlohmann::json j;
    try {
        f >> j;
    } catch (const std::exception& e) {
        return make_error("Malformed .fjclimate file " + path + ": " + e.what());
    }

    ClimateAsset asset;
    asset.name = j.value("name", std::string{});
    if (asset.name.empty()) {
        asset.name = std::filesystem::path(path).stem().string();
    }
    asset.initial = j.value("initial", std::string{});
    asset.seed = j.value("seed", 1U);
    if (j.contains("states") && j["states"].is_array()) {
        for (const auto& js : j["states"]) {
            ClimateState state;
            state.name = js.value("name", std::string{});
            state.event = js.value("event", std::string{});
            if (state.name.empty() || state.event.empty()) continue;
            state.weight = std::max(js.value("weight", 1.0f), 0.0f);
            state.from_hour = js.value("from_hour", 0.0f);
            state.to_hour = js.value("to_hour", 0.0f);
            if (js.contains("next") && js["next"].is_array()) {
                for (const auto& n : js["next"]) {
                    if (n.is_string()) state.next.push_back(n.get<std::string>());
                }
            }
            asset.states.push_back(std::move(state));
        }
    }
    return asset;
}

Result<> save_climate_asset_file(const std::string& path, const ClimateAsset& asset) {
    nlohmann::json j;
    j["name"] = asset.name;
    if (!asset.initial.empty()) j["initial"] = asset.initial;
    j["seed"] = asset.seed;
    auto& js = j["states"] = nlohmann::json::array();
    for (const auto& state : asset.states) {
        nlohmann::json entry;
        entry["name"] = state.name;
        entry["event"] = state.event;
        entry["weight"] = state.weight;
        if (state.from_hour != state.to_hour) {
            entry["from_hour"] = state.from_hour;
            entry["to_hour"] = state.to_hour;
        }
        if (!state.next.empty()) entry["next"] = state.next;
        js.push_back(std::move(entry));
    }

    std::ofstream f(path);
    if (!f.is_open()) {
        return make_error("Failed to write .fjclimate file: " + path);
    }
    f << j.dump(4) << "\n";
    return {};
}

} // namespace fjell
