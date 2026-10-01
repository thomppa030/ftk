#include "animation/anim_notify_registry.hpp"
#include "ftk/base/log.hpp"

namespace fjell {

AnimNotifyRegistry& AnimNotifyRegistry::instance() {
    static AnimNotifyRegistry registry;
    return registry;
}

void AnimNotifyRegistry::register_notify(NotifyTypeInfo info) {
    std::string name = info.name;
    entries_.insert_or_assign(std::move(name), std::move(info));
}

std::unique_ptr<AnimNotify> AnimNotifyRegistry::create(const std::string& type_name) const {
    auto it = entries_.find(type_name);
    if (it == entries_.end()) {
        FTK_CORE_WARN("AnimNotifyRegistry: unknown notify type '{}'", type_name);
        return nullptr;
    }
    return it->second.create();
}

const NotifyTypeInfo* AnimNotifyRegistry::find(const std::string& type_name) const {
    auto it = entries_.find(type_name);
    return it != entries_.end() ? &it->second : nullptr;
}

} // namespace fjell
