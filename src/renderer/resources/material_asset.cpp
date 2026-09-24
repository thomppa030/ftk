#include "renderer/resources/material_asset.hpp"
#include "core/log.hpp"
#include "core/math/color_space.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace fjell {

MaterialAsset load_material_asset(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        throw std::runtime_error("Failed to open .fjmat file: " + path);
    }
    std::stringstream text;
    text << f.rdbuf();
    return material_asset_from_text(text.str());
}

MaterialAsset material_asset_from_text(std::string_view text) {
    nlohmann::json j = nlohmann::json::parse(text);

    MaterialAsset asset;
    asset.shader = j.value("shader", "shaders/standard.fjsl");

    if (j.contains("textures")) {
        for (auto& [name, val] : j["textures"].items()) {
            asset.textures[name] = val.get<std::string>();
        }
    }

    if (j.contains("params")) {
        for (auto& [name, val] : j["params"].items()) {
            if (val.is_array()) {
                std::vector<float> values;
                for (auto& v : val) {
                    values.push_back(v.get<float>());
                }
                asset.params[name] = values;
            } else if (val.is_number()) {
                asset.params[name] = {val.get<float>()};
            }
        }
    }

    return asset;
}

std::string material_asset_text(const MaterialAsset& asset) {
    nlohmann::json j;
    j["shader"] = asset.shader;

    nlohmann::json tex_obj = nlohmann::json::object();
    for (const auto& [name, tex_path] : asset.textures) {
        tex_obj[name] = tex_path;
    }
    j["textures"] = tex_obj;

    nlohmann::json params_obj = nlohmann::json::object();
    for (const auto& [name, values] : asset.params) {
        if (values.size() == 1) {
            params_obj[name] = values[0];
        } else {
            params_obj[name] = values;
        }
    }
    j["params"] = params_obj;
    return j.dump(4) + "\n";
}

void save_material_asset(const std::string& path, const MaterialAsset& asset) {
    std::ofstream f(path);
    if (!f.is_open()) {
        throw std::runtime_error("Failed to write .fjmat file: " + path);
    }
    f << material_asset_text(asset);
}

namespace {

constexpr const char* BASE_COLOR_KEY = "base_color_factor";
constexpr const char* METALLIC_KEY = "metallic_factor";
constexpr const char* ROUGHNESS_KEY = "roughness_factor";
constexpr const char* UV_SCALE_KEY = "uv_scale";

float scalar_or(const MaterialAsset& asset, const char* key, float fallback) {
    auto it = asset.params.find(key);
    return it != asset.params.end() && !it->second.empty() ? it->second[0] : fallback;
}

// Compared before any conversion: a colour that round-trips through the
// sRGB curve lands a float away from 1.0, which is not a value worth saving.
void set_or_erase(MaterialAsset& asset, const char* key,
                  std::vector<float> value, bool is_default) {
    if (is_default) {
        asset.params.erase(key);
    } else {
        asset.params[key] = std::move(value);
    }
}

} // namespace

bool is_reserved_param(const std::string& key) {
    return key == BASE_COLOR_KEY || key == METALLIC_KEY
        || key == ROUGHNESS_KEY || key == UV_SCALE_KEY;
}

MaterialBaseParams read_base_params(const MaterialAsset& asset) {
    MaterialBaseParams base;
    auto bc = asset.params.find(BASE_COLOR_KEY);
    if (bc != asset.params.end() && bc->second.size() >= 3) {
        const auto& v = bc->second;
        base.base_color_factor = color_space::srgb_to_linear(
            glm::vec4{v[0], v[1], v[2], v.size() >= 4 ? v[3] : 1.0f});
    }
    base.metallic_factor = scalar_or(asset, METALLIC_KEY, base.metallic_factor);
    base.roughness_factor = scalar_or(asset, ROUGHNESS_KEY, base.roughness_factor);
    auto uv = asset.params.find(UV_SCALE_KEY);
    if (uv != asset.params.end() && uv->second.size() >= 2) {
        base.uv_scale = {uv->second[0], uv->second[1]};
    }
    return base;
}

void write_base_params(MaterialAsset& asset, const MaterialBaseParams& base) {
    const MaterialBaseParams defaults;
    const glm::vec4 bc = color_space::linear_to_srgb(base.base_color_factor);
    set_or_erase(asset, BASE_COLOR_KEY, {bc.r, bc.g, bc.b, bc.a},
                 base.base_color_factor == defaults.base_color_factor);
    set_or_erase(asset, METALLIC_KEY, {base.metallic_factor},
                 base.metallic_factor == defaults.metallic_factor);
    set_or_erase(asset, ROUGHNESS_KEY, {base.roughness_factor},
                 base.roughness_factor == defaults.roughness_factor);
    set_or_erase(asset, UV_SCALE_KEY, {base.uv_scale.x, base.uv_scale.y},
                 base.uv_scale == defaults.uv_scale);
}

} // namespace fjell
