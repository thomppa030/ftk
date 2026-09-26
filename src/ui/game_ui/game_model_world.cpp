#include "ui/game_ui/game_model_world.hpp"

#include "net/net_manager.hpp"
#include "scripting/script_manager.hpp"

namespace fjell {

std::optional<UiComponentType> GameModelWorld::component_type(const std::string& name) const {
    const auto* type = scripts_.is_loaded() ? scripts_.find_script_type(name) : nullptr;
    if (type == nullptr || !type->loaded) return std::nullopt;
    return UiComponentType{type->name, type->fields};
}

bool GameModelWorld::owned_by_local(uint32_t node_id) const {
    return net_ == nullptr || net_->is_owned_by_local(net_->net_id_for(node_id));
}

uint64_t GameModelWorld::possession_revision() const {
    return net_ != nullptr ? net_->possession_revision() : 0;
}

} // namespace fjell
