#include "renderer/vfx_asset.hpp"
#include "core/log.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>

namespace fjell {

VfxAsset load_vfx_asset_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        throw std::runtime_error("Failed to open .fjvfx file: " + path);
    }

    nlohmann::json j;
    f >> j;

    VfxAsset asset;
    asset.emission_rate = j.value("emission_rate", 50.0f);
    asset.lifetime_min = j.value("lifetime_min", 1.0f);
    asset.lifetime_max = j.value("lifetime_max", 3.0f);
    asset.max_particles = j.value("max_particles", 10000u);

    if (j.contains("velocity_min")) {
        auto& v = j["velocity_min"];
        asset.velocity_min = {v[0].get<float>(), v[1].get<float>(), v[2].get<float>()};
    }
    if (j.contains("velocity_max")) {
        auto& v = j["velocity_max"];
        asset.velocity_max = {v[0].get<float>(), v[1].get<float>(), v[2].get<float>()};
    }
    if (j.contains("gravity")) {
        auto& g = j["gravity"];
        asset.gravity = {g[0].get<float>(), g[1].get<float>(), g[2].get<float>()};
    }
    if (j.contains("color_start")) {
        auto& c = j["color_start"];
        asset.color_start = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c[3].get<float>()};
    }
    if (j.contains("color_end")) {
        auto& c = j["color_end"];
        asset.color_end = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c[3].get<float>()};
    }

    asset.size_start = j.value("size_start", 0.1f);
    asset.size_end = j.value("size_end", 0.02f);
    asset.blend_mode = static_cast<ParticleBlendMode>(j.value("blend_mode", 0));

    return asset;
}

void save_vfx_asset_file(const std::string& path, const VfxAsset& asset) {
    nlohmann::json j;
    j["emission_rate"] = asset.emission_rate;
    j["lifetime_min"] = asset.lifetime_min;
    j["lifetime_max"] = asset.lifetime_max;
    j["max_particles"] = asset.max_particles;
    j["velocity_min"] = {asset.velocity_min.x, asset.velocity_min.y, asset.velocity_min.z};
    j["velocity_max"] = {asset.velocity_max.x, asset.velocity_max.y, asset.velocity_max.z};
    j["gravity"] = {asset.gravity.x, asset.gravity.y, asset.gravity.z};
    j["color_start"] = {asset.color_start.r, asset.color_start.g, asset.color_start.b, asset.color_start.a};
    j["color_end"] = {asset.color_end.r, asset.color_end.g, asset.color_end.b, asset.color_end.a};
    j["size_start"] = asset.size_start;
    j["size_end"] = asset.size_end;
    j["blend_mode"] = static_cast<int>(asset.blend_mode);

    std::ofstream f(path);
    if (!f.is_open()) {
        throw std::runtime_error("Failed to write .fjvfx file: " + path);
    }
    f << j.dump(4) << "\n";
}

void apply_vfx_to_emitter(const VfxAsset& asset, EmitterComponent& em) {
    em.emission_rate = asset.emission_rate;
    em.lifetime_min = asset.lifetime_min;
    em.lifetime_max = asset.lifetime_max;
    em.max_particles = asset.max_particles;
    em.velocity_min = asset.velocity_min;
    em.velocity_max = asset.velocity_max;
    em.gravity = asset.gravity;
    em.color_start = asset.color_start;
    em.color_end = asset.color_end;
    em.size_start = asset.size_start;
    em.size_end = asset.size_end;
    em.blend_mode = asset.blend_mode;
}

} // namespace fjell
