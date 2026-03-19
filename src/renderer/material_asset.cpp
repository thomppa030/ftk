#include "renderer/material_asset.hpp"
#include "core/log.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>

namespace fjell {

MaterialAsset load_material_asset(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        throw std::runtime_error("Failed to open .fjmat file: " + path);
    }

    nlohmann::json j;
    f >> j;

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

void save_material_asset(const std::string& path, const MaterialAsset& asset) {
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

    std::ofstream f(path);
    if (!f.is_open()) {
        throw std::runtime_error("Failed to write .fjmat file: " + path);
    }
    f << j.dump(4) << "\n";
}

} // namespace fjell
