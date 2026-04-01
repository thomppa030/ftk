#pragma once

#include "animation/anim_notify.hpp"
#include "scripting/script_manager.hpp" // OwnedFieldDesc

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fjell {

struct NotifyTypeInfo {
    std::string name;
    std::function<std::unique_ptr<AnimNotify>()> create;
    std::vector<OwnedFieldDesc> fields;
    void (*trace)(void*, void*){nullptr};
    bool is_state{false};
};

// Singleton registry mapping notify type names to factories and field descriptors.
// Populated at static init time (engine built-ins) and when game .so loads (FJELL_NOTIFY types).
class AnimNotifyRegistry {
public:
    [[nodiscard]] static AnimNotifyRegistry& instance();

    void register_notify(NotifyTypeInfo info);

    [[nodiscard]] std::unique_ptr<AnimNotify> create(const std::string& type_name) const;
    [[nodiscard]] const NotifyTypeInfo* find(const std::string& type_name) const;

    template <typename Fn>
    void for_each(const Fn& fn) const {
        for (const auto& [name, info] : entries_) {
            fn(name, info);
        }
    }

private:
    std::unordered_map<std::string, NotifyTypeInfo> entries_;
};

} // namespace fjell
