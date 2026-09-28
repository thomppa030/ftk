#include "scripting/script_globals.hpp"
#include "core/asset_type_registry.hpp"
#include "scripting/fjell/asset.hpp"

namespace fjell {

static ScriptServices s_services;

ScriptServices& script_services() { return s_services; }

AssetData script_asset(std::string_view type_name, uint32_t handle_id) {
    if (handle_id == 0) return {};
    const AssetTypeEntry* entry = AssetTypeRegistry::instance().find_by_name(type_name);
    return {.handle_id = handle_id, .asset_type_id = entry != nullptr ? entry->type_id : AssetTypeId{0}};
}

} // namespace fjell
