#include "audio/audio_asset.hpp"
#include "audio/audio_component.hpp"

#include <nlohmann/json.hpp>

#include <fstream>

namespace fjell {

ftk::Result<AudioAsset> load_audio_asset_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        return ftk::make_error("Failed to open .fjaudio file: " + path);
    }

    nlohmann::json j;
    f >> j;

    AudioAsset asset;
    asset.clip_path = j.value("clip", std::string{});
    asset.volume = j.value("volume", 1.0f);
    asset.pitch = j.value("pitch", 1.0f);
    asset.min_distance = j.value("min_distance", 1.0f);
    asset.max_distance = j.value("max_distance", 50.0f);
    asset.loop = j.value("loop", false);
    asset.spatial = j.value("spatial", true);
    return asset;
}

ftk::Result<> save_audio_asset_file(const std::string& path, const AudioAsset& asset) {
    nlohmann::json j;
    j["clip"] = asset.clip_path;
    j["volume"] = asset.volume;
    j["pitch"] = asset.pitch;
    j["min_distance"] = asset.min_distance;
    j["max_distance"] = asset.max_distance;
    j["loop"] = asset.loop;
    j["spatial"] = asset.spatial;

    std::ofstream f(path);
    if (!f.is_open()) {
        return ftk::make_error("Failed to write .fjaudio file: " + path);
    }
    f << j.dump(4) << "\n";
    return {};
}

void apply_audio_to_source(const AudioAsset& asset, AudioSourceComponent& source) {
    source.clip_path = asset.clip_path;
    source.volume = asset.volume;
    source.pitch = asset.pitch;
    source.min_distance = asset.min_distance;
    source.max_distance = asset.max_distance;
    source.loop = asset.loop;
    source.spatial = asset.spatial;
}

} // namespace fjell
