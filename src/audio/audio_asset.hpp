#pragma once

#include "ftk/base/result.hpp"

#include <string>

namespace fjell {

struct AudioSourceComponent;

struct AudioAsset {
    std::string clip_path;
    float volume{1.0f};
    float pitch{1.0f};
    float min_distance{1.0f};
    float max_distance{50.0f};
    bool loop{false};
    bool spatial{true};
};

[[nodiscard]] ftk::Result<AudioAsset> load_audio_asset_file(const std::string& path);
[[nodiscard]] ftk::Result<> save_audio_asset_file(const std::string& path, const AudioAsset& asset);
void apply_audio_to_source(const AudioAsset& asset, AudioSourceComponent& source);

} // namespace fjell
