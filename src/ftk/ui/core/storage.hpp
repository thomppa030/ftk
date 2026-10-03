#pragma once

// What the UI keeps for a widget from one frame to the next, under its id:
// a fold, the row picked, a width last measured. Kept for the session, not
// saved. A value is read back as the kind it was kept as; read as another
// kind it is the fallback.
//
//     const bool open = storage.get_bool(fold, true);
//     if (clicked) storage.set_bool(fold, !open);

#include "ftk/ui/core/types.hpp"

#include <unordered_map>
#include <variant>

namespace ftk::ui::core {

class Storage {
public:
    /// What is kept under `id`, or `fallback` when nothing of that kind is.
    [[nodiscard]] int get_int(Id id, int fallback = 0) const { return get(id, fallback); }
    [[nodiscard]] float get_float(Id id, float fallback = 0.0f) const { return get(id, fallback); }
    [[nodiscard]] bool get_bool(Id id, bool fallback = false) const { return get(id, fallback); }

    /// Keeps `value` under `id`, in place of whatever was.
    void set_int(Id id, int value) { values_[id] = value; }
    void set_float(Id id, float value) { values_[id] = value; }
    void set_bool(Id id, bool value) { values_[id] = value; }

    [[nodiscard]] bool contains(Id id) const { return values_.contains(id); }
    void erase(Id id) { values_.erase(id); }
    [[nodiscard]] std::size_t size() const { return values_.size(); }

private:
    using Value = std::variant<int, float, bool>;

    template <typename T>
    [[nodiscard]] T get(Id id, T fallback) const {
        const auto found = values_.find(id);
        if (found == values_.end()) return fallback;
        const T* value = std::get_if<T>(&found->second);
        return value != nullptr ? *value : fallback;
    }

    std::unordered_map<Id, Value> values_;
};

} // namespace ftk::ui::core
