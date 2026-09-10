#include "core/asset_type_registry.hpp"

namespace fjell {

AssetTypeRegistry& AssetTypeRegistry::instance() {
    static AssetTypeRegistry registry;
    return registry;
}

void AssetTypeRegistry::register_type(AssetTypeEntry entry) {
    entry.type_id = next_id_++;
    entries_.push_back(std::move(entry));
}

const AssetTypeEntry* AssetTypeRegistry::find_by_name(std::string_view name) const {
    for (const auto& e : entries_) {
        if (e.type_name == name) return &e;
    }
    return nullptr;
}

const AssetTypeEntry* AssetTypeRegistry::find_by_id(AssetTypeId id) const {
    for (const auto& e : entries_) {
        if (e.type_id == id) return &e;
    }
    return nullptr;
}

} // namespace fjell
