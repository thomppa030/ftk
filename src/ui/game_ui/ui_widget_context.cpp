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

FJELL_SDK_EXPORT void UiWidgetContext::set_list_size(const std::string& key, std::size_t count) {
    if (ctx_) ctx_->set_list_size(key, count);
}

FJELL_SDK_EXPORT void UiWidgetContext::set_item(const std::string& key, std::size_t index,
                                                const std::string& field, const std::string& value) {
    if (ctx_) ctx_->set_item(key, index, field, value);
}

FJELL_SDK_EXPORT void UiWidgetContext::bind_action(const std::string& name,
                                   std::function<void()> callback) {
    if (!ctx_ || !connections_) return;
    // Wrap the script callback in engine-allocated shared_ptr so the Delegate's
    // move_only_function doesn't try to free game.so memory on destruction.
    auto shared_cb = std::make_shared<std::function<void()>>(std::move(callback));
    connections_->push_back(ctx_->bind_action(name, [shared_cb]() {
        (*shared_cb)();
    }));
}

FJELL_SDK_EXPORT void UiWidgetContext::bind_action(const std::string& name,
                                   std::function<void(const std::vector<std::string>&)> callback) {
    if (!ctx_ || !connections_) return;
    auto shared_cb = std::make_shared<std::function<void(const std::vector<std::string>&)>>(
        std::move(callback));
    connections_->push_back(ctx_->bind_action(name,
        [shared_cb](const std::vector<std::string>& args) {
            (*shared_cb)(args);
        }));
}

} // namespace fjell
