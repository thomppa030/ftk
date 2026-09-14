#pragma once

#include <glm/glm.hpp>

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

/// Factors every material carries regardless of its shader: the base PBR
/// multipliers and the UV scale. They live in `params` under these reserved
/// names next to the shader's own uniforms, and a value at its default is
/// left out of the file so a hand-written material stays short.
struct MaterialBaseParams {
    float metallic_factor{1.0f};
    float roughness_factor{1.0f};
    glm::vec2 uv_scale{1.0f, 1.0f};
};

[[nodiscard]] MaterialBaseParams read_base_params(const MaterialAsset& asset);
void write_base_params(MaterialAsset& asset, const MaterialBaseParams& base);

} // namespace fjell
