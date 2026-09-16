#pragma once

#include "scene/component_meta.hpp"
#include "core/asset_type_registry.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fjell {

class Project;

struct AssetRefConfig {
    const char* label;
    const char* id;                              // ImGui ID (e.g., "##MatRef")
    std::span<const std::string> extensions;     // accepted file extensions
    const InspectorContext* ctx{nullptr};         // for content browser, file browser, icons
    const AssetTypeEntry* asset_type{nullptr};    // optional, for draw_preview
};

/// Unified asset reference widget.
/// Draws: label, read-only display field, drag-drop target, [Use Selected], [Browse], [Clear].
/// Returns true if path was changed.
bool asset_ref_field(const AssetRefConfig& config, std::string& path);

/// Every file under the project with this extension, as project-relative
/// paths in sorted order. A directory walk; callers that draw every frame
/// cache the result and refresh it now and then.
[[nodiscard]] std::vector<std::string> list_project_assets(const Project* project,
                                                            std::string_view extension);

/// Call this when the inspector FileBrowser fires on_selected.
/// Converts absolute path to project-relative and delivers to the pending widget.
void asset_ref_browse_accept(const std::string& absolute_path,
                             const std::string& project_root);

} // namespace fjell
