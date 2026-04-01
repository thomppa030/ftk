#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace fjell {

// Deserialized .fjmat file
struct MaterialAsset {
    std::string shader; // e.g. "shaders/standard.fjsl"
    std::unordered_map<std::string, std::string> textures; // name -> path
    std::unordered_map<std::string, std::vector<float>> params; // name -> value(s)
};

// Load a .fjmat JSON file from disk.
[[nodiscard]] MaterialAsset load_material_asset(const std::string& path);

// Save a .fjmat JSON file to disk.
void save_material_asset(const std::string& path, const MaterialAsset& asset);

} // namespace fjell
