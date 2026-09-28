#pragma once

#include "scene/component_meta.hpp"
#include "core/asset_type_registry.hpp"
#include "renderer/resources/fjsl_parser.hpp"

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
    const InspectorContext* ctx{nullptr};         // for the project, content browser, icons
    const AssetTypeEntry* asset_type{nullptr};    // optional, for draw_preview
    /// For a slot taking shaders or materials: the Types it can draw. Others
    /// are not offered, not accepted, and named under the field when one is
    /// already there. Empty takes any.
    std::span<const ShaderType> shader_types{};
    /// The file the slot holds failed to load: it reads as missing, as a file
    /// that is gone does, though it is there.
    bool failed{false};
};

/// A field holding an asset reference, for the value column of a property
/// row: the asset's kind icon and name (the path in the tooltip), "None" when
/// empty and "missing: name" when the file is gone or failed to load
/// (`config.failed`). Clicking it opens a
/// searchable picker of every asset the field takes, the content browser's
/// selection first; an asset dragged onto it is taken if it fits and refused,
/// with the reason, if it doesn't. A clear button follows it, disabled while
/// it is empty so the field keeps its width. `config.label` is not drawn: the row names the field. Returns
/// true when `path` changed; every change is a finished edit.
bool asset_slot(const AssetRefConfig& config, std::string& path);

/// The slot under its own label, for panels not yet on property rows.
bool asset_ref_field(const AssetRefConfig& config, std::string& path);

/// Every file under the project with this extension, as project-relative
/// paths in sorted order. A directory walk; callers that draw every frame
/// cache the result and refresh it now and then.
[[nodiscard]] std::vector<std::string> list_project_assets(const Project* project,
                                                            std::string_view extension);

} // namespace fjell
