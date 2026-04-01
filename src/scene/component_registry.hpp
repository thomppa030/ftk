#pragma once

#include "scene/component_meta.hpp"
#include "scene/component_storage.hpp"

#include <string_view>
#include <vector>

namespace fjell {

class ComponentRegistry {
public:
    static ComponentRegistry& instance();

    template <typename T>
    void register_component(ComponentMeta meta) {
        entries_.push_back({component_type_id<T>(), std::move(meta)});
    }

    [[nodiscard]] const ComponentMeta* find(ComponentTypeId id) const {
        for (const auto& e : entries_) {
            if (e.id == id) return &e.meta;
        }
        return nullptr;
    }

    [[nodiscard]] const ComponentMeta* find_by_key(std::string_view key) const {
        for (const auto& e : entries_) {
            if (e.meta.json_key == key) return &e.meta;
        }
        return nullptr;
    }

    template <typename Fn>
    void for_each(const Fn& fn) const {
        for (const auto& e : entries_) {
            fn(e.id, e.meta);
        }
    }

private:
    struct Entry {
        ComponentTypeId id;
        ComponentMeta meta;
    };

    std::vector<Entry> entries_;
};

} // namespace fjell
