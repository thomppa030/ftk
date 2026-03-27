#include "ui/context_registry.hpp"

namespace fjell {

void ContextRegistry::register_type(const std::string& asset_type, ContextFactoryEntry entry) {
    factories_[asset_type] = std::move(entry);
}

const ContextFactoryEntry* ContextRegistry::find(const std::string& asset_type) const {
    auto it = factories_.find(asset_type);
    return it != factories_.end() ? &it->second : nullptr;
}

} // namespace fjell
