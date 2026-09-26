#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
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

class ComponentStorage;

// How a component is duplicated when its node is. The default copy-constructs,
// which is right for plain data. A type whose copy would be wrong — runtime
// handles that must not be shared, state that has to be rebuilt against a
// sibling component, or no copy constructor at all — declares an overload
// next to its own definition. `from` is the storage of the node being copied,
// for rules that need a sibling. A type without a copy constructor and without
// an overload fails to compile, so no component can be forgotten.
template <typename T>
void clone_component(const ComponentStorage& from, const T& src, ComponentStorage& to);

/// What code that holds components without knowing their type needs to know
/// about one: its id, the memory it takes, how to end it and how to copy it by
/// its clone rule. There is one per component type, made the first time a
/// component of the type is created. Looking a component up compares ids and
/// never needs this, so code that only reads a type does not pull in how to
/// destroy or copy it.
struct ComponentType {
    ComponentTypeId id;
    uint32_t size;
    uint32_t align;
    /// Runs the component's destructor. The memory stays with its owner.
    void (*destroy)(void* component);
    void (*clone)(const ComponentStorage& from, const void* src, ComponentStorage& to);
};

template <typename T>
const ComponentType& component_type() {
    // Components live in plain byte allocations, which are aligned this far
    // and no further.
    static_assert(alignof(T) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__,
                  "a component may not need more alignment than operator new gives");
    static const ComponentType type{
        component_type_id<T>(),
        static_cast<uint32_t>(sizeof(T)),
        static_cast<uint32_t>(alignof(T)),
        [](void* component) { std::destroy_at(static_cast<T*>(component)); },
        [](const ComponentStorage& from, const void* src, ComponentStorage& to) {
            clone_component(from, *static_cast<const T*>(src), to);
        },
    };
    return type;
}

/// One component in memory of its own. The box owns it: destroying the box
/// ends the component and frees the memory, and moving the box hands both
/// over without touching the component, so its address never changes.
class ComponentBox {
public:
    template <typename T, typename... Args>
    [[nodiscard]] static ComponentBox make(Args&&... args) {
        const ComponentType& type = component_type<T>();
        ComponentBox box;
        box.type_ = &type;
        box.memory_ = std::make_unique_for_overwrite<std::byte[]>(type.size);
        box.component_ = std::construct_at(reinterpret_cast<T*>(box.memory_.get()),
                                           std::forward<Args>(args)...);
        return box;
    }

    ComponentBox() = default;
    ~ComponentBox() { reset(); }
    ComponentBox(const ComponentBox&) = delete;
    ComponentBox& operator=(const ComponentBox&) = delete;
    ComponentBox(ComponentBox&& other) noexcept
        : type_{other.type_}, memory_{std::move(other.memory_)}, component_{other.component_} {
        other.component_ = nullptr;
    }
    ComponentBox& operator=(ComponentBox&& other) noexcept {
        if (this != &other) {
            reset();
            type_ = other.type_;
            memory_ = std::move(other.memory_);
            component_ = other.component_;
            other.component_ = nullptr;
        }
        return *this;
    }

    /// False for a default-made box, and for one moved from.
    [[nodiscard]] explicit operator bool() const { return component_ != nullptr; }
    [[nodiscard]] const ComponentType& type() const { return *type_; }
    [[nodiscard]] void* get() const { return component_; }

private:
    void reset() {
        if (component_ != nullptr) type_->destroy(component_);
        component_ = nullptr;
        memory_.reset();
    }

    const ComponentType* type_{nullptr};
    std::unique_ptr<std::byte[]> memory_;
    void* component_{nullptr};
};

// Type-erased component storage.
// Holds any mix of component types, queryable by compile-time type ID.
// Optimized for small counts (1-4 components per node) via linear scan.
class ComponentStorage {
public:
    /// A component taken out of a storage by extract(), still alive and
    /// unchanged, until it is put back with restore(). This is how removing
    /// a component is undone: the same object returns, with every field and
    /// any runtime state it held, instead of a copy rebuilt from JSON.
    /// Destroying one that was never restored destroys the component.
    class Detached {
    public:
        /// False when extract() found no component of the type, or once
        /// the component has been restored.
        [[nodiscard]] explicit operator bool() const { return static_cast<bool>(box_); }
        [[nodiscard]] ComponentTypeId type_id() const { return box_ ? box_.type().id : 0; }

    private:
        friend class ComponentStorage;
        ComponentBox box_;
        size_t index_{0};
    };

    ComponentStorage() = default;
    ComponentStorage(const ComponentStorage&) = delete;
    ComponentStorage& operator=(const ComponentStorage&) = delete;
    ComponentStorage(ComponentStorage&&) noexcept = default;
    ComponentStorage& operator=(ComponentStorage&&) noexcept = default;
    ~ComponentStorage() = default;

    template <typename T>
    T* get() {
        return static_cast<T*>(get_by_id(component_type_id<T>()));
    }

    template <typename T>
    const T* get() const {
        return static_cast<const T*>(get_by_id(component_type_id<T>()));
    }

    template <typename T, typename... Args>
    T& add(Args&&... args) {
        entries_.push_back(ComponentBox::make<T>(std::forward<Args>(args)...));
        return *static_cast<T*>(entries_.back().get());
    }

    // Duplicates every component into `to`, each by its own clone rule.
    void clone_into(ComponentStorage& to) const {
        for (const auto& e : entries_) {
            e.type().clone(*this, e.get(), to);
        }
    }

    template <typename T>
    void remove() {
        (void)remove_by_id(component_type_id<T>());
    }

    template <typename T>
    [[nodiscard]] bool has() const {
        return get_by_id(component_type_id<T>()) != nullptr;
    }

    // Runtime lookup by type ID (for script components, serialization)
    [[nodiscard]] void* get_by_id(ComponentTypeId id) {
        for (auto& e : entries_) {
            if (e.type().id == id) return e.get();
        }
        return nullptr;
    }

    [[nodiscard]] const void* get_by_id(ComponentTypeId id) const {
        for (const auto& e : entries_) {
            if (e.type().id == id) return e.get();
        }
        return nullptr;
    }

    // Remove by runtime type ID
    bool remove_by_id(ComponentTypeId id) {
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->type().id == id) {
                // Swap-and-pop for O(1) removal
                if (it != entries_.end() - 1) *it = std::move(entries_.back());
                entries_.pop_back();
                return true;
            }
        }
        return false;
    }

    /// Takes the component of type `id` out without destroying it. The
    /// others keep their order, and restore() puts it back where it was, so
    /// an undone removal doesn't reorder the inspector. Empty when the
    /// storage has no component of that type.
    [[nodiscard]] Detached extract(ComponentTypeId id) {
        Detached out;
        for (size_t i = 0; i < entries_.size(); ++i) {
            if (entries_[i].type().id == id) {
                out.box_ = std::move(entries_[i]);
                out.index_ = i;
                entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
        }
        return out;
    }

    /// Position of the component of type `id` among the others, or count()
    /// when there is none.
    [[nodiscard]] size_t index_of(ComponentTypeId id) const {
        for (size_t i = 0; i < entries_.size(); ++i) {
            if (entries_[i].type().id == id) return i;
        }
        return entries_.size();
    }

    /// Adds a copy of this storage's component of type `id` to `to`, made by
    /// its clone rule. `siblings` is the storage the rule may consult for the
    /// component's siblings: this one for a live node, the live node's for a
    /// copy kept aside. Does nothing when there is no such component.
    void clone_one(ComponentTypeId id, const ComponentStorage& siblings, ComponentStorage& to) const {
        for (const auto& e : entries_) {
            if (e.type().id == id) {
                e.type().clone(siblings, e.get(), to);
                return;
            }
        }
    }

    /// Puts an extracted component back at its old position. Does nothing
    /// for an empty Detached, or when a component of the same type has been
    /// added since, which then keeps its place and `detached` its component.
    void restore(Detached&& detached) {
        if (!detached || get_by_id(detached.type_id()) != nullptr) return;
        const size_t at = detached.index_ < entries_.size() ? detached.index_ : entries_.size();
        entries_.insert(entries_.begin() + static_cast<std::ptrdiff_t>(at),
                        std::move(detached.box_));
    }

    /// Puts an extracted component in at position `at`, which may come from
    /// another storage. The same rules as restore() otherwise.
    void restore(Detached&& detached, size_t at) {
        detached.index_ = at;
        restore(std::move(detached));
    }

    // Iterate all components (for serialization, cloning, inspector)
    template <typename Fn>
    void for_each(const Fn& fn) {
        for (auto& e : entries_) {
            fn(e.type().id, e.get());
        }
    }

    template <typename Fn>
    void for_each(const Fn& fn) const {
        for (const auto& e : entries_) {
            fn(e.type().id, e.get());
        }
    }

    [[nodiscard]] size_t count() const { return entries_.size(); }
    [[nodiscard]] bool empty() const { return entries_.empty(); }

    void clear() { entries_.clear(); }

private:
    std::vector<ComponentBox> entries_;
};

template <typename T>
void clone_component(const ComponentStorage& /*from*/, const T& src, ComponentStorage& to) {
    to.add<T>(src);
}

} // namespace fjell
