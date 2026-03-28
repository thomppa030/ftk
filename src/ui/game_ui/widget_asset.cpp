#include "ui/game_ui/widget_asset.hpp"
#include "core/log.hpp"

#include <nlohmann/json.hpp>

#include <fstream>

namespace fjell {

Result<WidgetAsset> load_widget_asset(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return make_error("Widget asset not found: " + path);
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(file);
    } catch (const nlohmann::json::parse_error& e) {
        return make_error(std::string("Failed to parse widget asset: ") + e.what());
    }

    WidgetAsset asset;
    asset.class_name = j.value("class", "");
    asset.fjui_path = j.value("ui", "");
    asset.fjss_path = j.value("style", "");

    if (asset.class_name.empty()) {
        FJELL_CORE_WARN("Widget asset has no 'class' field: {}", path);
    }
    if (asset.fjui_path.empty()) {
        return make_error("Widget asset missing 'ui' field: " + path);
    }

    return asset;
}

Result<> save_widget_asset(const std::string& path, const WidgetAsset& asset) {
    nlohmann::json j;
    j["class"] = asset.class_name;
    j["ui"] = asset.fjui_path;
    if (!asset.fjss_path.empty()) {
        j["style"] = asset.fjss_path;
    }

    std::ofstream file(path);
    if (!file.is_open()) {
        return make_error("Failed to save widget asset: " + path);
    }
    file << j.dump(2);
    return {};
}

} // namespace fjell
