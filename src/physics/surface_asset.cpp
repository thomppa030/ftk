#include "physics/surface_asset.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>

namespace fjell {

ftk::Result<SurfaceAsset> load_surface_asset_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        return ftk::make_error("Failed to open .fjsurface file: " + path);
    }

    nlohmann::json j;
    try {
        f >> j;
    } catch (const std::exception& e) {
        return ftk::make_error("Malformed .fjsurface file " + path + ": " + e.what());
    }

    SurfaceAsset asset;
    asset.name = j.value("name", std::string{});
    if (asset.name.empty()) {
        asset.name = std::filesystem::path(path).stem().string();
    }
    return asset;
}

ftk::Result<> save_surface_asset_file(const std::string& path, const SurfaceAsset& asset) {
    nlohmann::json j;
    j["name"] = asset.name;

    std::ofstream f(path);
    if (!f.is_open()) {
        return ftk::make_error("Failed to write .fjsurface file: " + path);
    }
    f << j.dump(4) << "\n";
    return {};
}

} // namespace fjell
