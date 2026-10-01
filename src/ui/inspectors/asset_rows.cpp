#include "ui/inspectors/asset_rows.hpp"

#include "core/asset_type_registry.hpp"
#include "ftk/ui/kit/row.hpp"
#include "renderer/resources/asset_shader_type.hpp"
#include "ui/asset_ref_widget.hpp"

namespace fjell {

bool material_row(const char* id, std::string& name, const InspectorContext& ctx) {
    static const std::string material_exts[] = {".fjmat"};
    const auto* mat_type = AssetTypeRegistry::instance().find_by_name("Material");
    bool changed = false;
    ftk::ui::row("Material", [&] {
        std::string shown = name == DEFAULT_MATERIAL ? std::string{} : name;
        if (asset_slot({.label = "Material", .id = id, .extensions = material_exts, .ctx = &ctx,
                        .asset_type = mat_type, .shader_types = only_shader_type(ShaderType::Spatial)},
                       shown)) {
            name = shown.empty() ? std::string(DEFAULT_MATERIAL) : shown;
            changed = true;
        }
    });
    return changed;
}

} // namespace fjell
