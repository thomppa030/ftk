#pragma once

#include "core/result.hpp"

#include <string>

namespace fjell {

struct WidgetAsset {
    std::string class_name;  // registered UiWidget type name
    std::string fjui_path;   // relative path to .fjui template
    std::string fjss_path;   // relative path to .fjss stylesheet (can be empty)
};

[[nodiscard]] Result<WidgetAsset> load_widget_asset(const std::string& path);
[[nodiscard]] Result<> save_widget_asset(const std::string& path, const WidgetAsset& asset);

} // namespace fjell
