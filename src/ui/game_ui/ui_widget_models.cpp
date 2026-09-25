#include "fjell/api.h"
#include "scripting/fjell/component.hpp"
#include "scripting/fjell/ui_widget.hpp"
#include "ui/game_ui/ui_models.hpp"

namespace fjell {

FJELL_SDK_EXPORT void UiWidgetModels::bind(const std::string& name, uint32_t node_id) {
    if (models_ != nullptr && scope_ != nullptr) (void)models_->bind_node(*scope_, name, node_id);
}

FJELL_SDK_EXPORT void UiWidgetModels::clear(const std::string& name) {
    if (models_ != nullptr && scope_ != nullptr) (void)models_->bind_node(*scope_, name, 0);
}

FJELL_SDK_EXPORT const ScriptComponent* UiWidgetModels::component(const std::string& name,
                                                                   const char* type_name) const {
    if (models_ == nullptr || scope_ == nullptr || type_name == nullptr) return nullptr;
    // A Model's component is a script's instance, stored as its base.
    return static_cast<const ScriptComponent*>(models_->component(*scope_, name, type_name));
}

} // namespace fjell
