#include "fjell/api.h"
#include "scripting/fjell/ui_widget.hpp"
#include "ui/game_ui/ui_context.hpp"

namespace fjell {

FJELL_SDK_EXPORT void UiWidgetContext::set(const std::string& key, const std::string& value) {
    if (ctx_) ctx_->set(key, value);
}

FJELL_SDK_EXPORT std::string UiWidgetContext::get(const std::string& key) const {
    if (ctx_) return ctx_->get(key);
    return "";
}

} // namespace fjell
