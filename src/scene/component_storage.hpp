#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace fjell {

using ComponentTypeId = uint32_t;

// Global counter for compile-time type IDs.
// Each unique type gets a unique ID on first call to component_type_id<T>().
inline ComponentTypeId next_component_id() {
    static ComponentTypeId counter{0};
    return counter++;
}

template <typename T>
ComponentTypeId component_type_id() {
    static const ComponentTypeId id = next_component_id();
    return id;
}

// Type-erased component storage.
// Holds any mix of component types, queryable by compile-time type ID.
// Optimized for small counts (1-4 components per node) via linear scan.
class ComponentStorage {
public:
    ComponentStorage() = default;
    ~ComponentStorage() { clear(); }

    ComponentStorage(const ComponentStorage&) = delete;
    ComponentStorage& operator=(const ComponentStorage&) = delete;

    ComponentStorage(ComponentStorage&& other) noexcept
        : entries_{std::move(other.entries_)} {}

    ComponentStorage& operator=(ComponentStorage&& other) noexcept {
        if (this != &other) {
            clear();
            entries_ = std::move(other.entries_);
        }
        return *this;
    }

    template <typename T>
    T* get() {
        auto id = component_type_id<T>();
        for (auto& e : entries_) {
            if (e.type_id == id) return static_cast<T*>(e.ptr);
        }
        return nullptr;
    }

    template <typename T>
    const T* get() const {
        auto id = component_type_id<T>();
        for (const auto& e : entries_) {
            if (e.type_id == id) return static_cast<const T*>(e.ptr);
        }
        return nullptr;
    }

    template <typename T, typename... Args>
    T& add(Args&&... args) {
        auto* ptr = new T(std::forward<Args>(args)...);
        entries_.push_back({
            component_type_id<T>(),
            ptr,
            [](void* p) { delete static_cast<T*>(p); },
            sizeof(T)
        });
        return *ptr;
    }

    template <typename T>
    void remove() {
        auto id = component_type_id<T>();
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->type_id == id) {
                it->deleter(it->ptr);
                // Swap-and-pop for O(1) removal
                *it = entries_.back();
                entries_.pop_back();
                return;
            }
        }
    }

    template <typename T>
    [[nodiscard]] bool has() const {
        auto id = component_type_id<T>();
        for (const auto& e : entries_) {
            if (e.type_id == id) return true;
        }
        return false;
    }

    // Runtime lookup by type ID (for script components, serialization)
    [[nodiscard]] void* get_by_id(ComponentTypeId id) {
        for (auto& e : entries_) {
            if (e.type_id == id) return e.ptr;
        }
        return nullptr;
    }

    [[nodiscard]] const void* get_by_id(ComponentTypeId id) const {
        for (const auto& e : entries_) {
            if (e.type_id == id) return e.ptr;
        }
        return nullptr;
    }

    // Remove by runtime type ID
    bool remove_by_id(ComponentTypeId id) {
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->type_id == id) {
                it->deleter(it->ptr);
                *it = entries_.back();
                entries_.pop_back();
                return true;
            }
        }
        return false;
    }

    // Iterate all components (for serialization, cloning, inspector)
    template <typename Fn>
    void for_each(const Fn& fn) {
        for (auto& e : entries_) {
            fn(e.type_id, e.ptr);
        }
    }

    template <typename Fn>
    void for_each(const Fn& fn) const {
        for (const auto& e : entries_) {
            fn(e.type_id, e.ptr);
        }
    }

    [[nodiscard]] size_t count() const { return entries_.size(); }
    [[nodiscard]] bool empty() const { return entries_.empty(); }

    void clear() {
        for (auto& e : entries_) {
            e.deleter(e.ptr);
        }
        entries_.clear();
    }

private:
    struct Entry {
        ComponentTypeId type_id;
        void* ptr;
        void (*deleter)(void*);
        uint32_t size; // for cloning/serialization if needed
    };

    std::vector<Entry> entries_;
};

} // namespace fjell
