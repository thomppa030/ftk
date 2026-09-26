#pragma once

#include "scene/component_pool.hpp"
#include "scene/component_type.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace fjell {

struct Node;

/// One component, owned. It lives either in memory of its own (a node that is
/// in no scene) or in a slot of its scene's pool for its type. Destroying the
/// box ends the component and gives the memory back; moving the box hands both
/// over without touching the component, so its address does not change. It
/// changes only when into_pool() or into_own_memory() moves it.
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

    template <typename T, typename... Args>
    [[nodiscard]] static ComponentBox make_in(ComponentPool& pool, Node& owner, Args&&... args) {
        ComponentBox box;
        box.type_ = &component_type<T>();
        const ComponentPool::Slot slot = pool.acquire(owner);
        box.pool_ = &pool;
        box.slot_ = slot.index;
        try {
            box.component_ = std::construct_at(static_cast<T*>(slot.memory),
                                               std::forward<Args>(args)...);
        } catch (...) {
            pool.release(slot.index);
            box.pool_ = nullptr;
            throw;
        }
        pool.admit(slot.index);
        return box;
    }

    ComponentBox() = default;
    ~ComponentBox() { reset(); }
    ComponentBox(const ComponentBox&) = delete;
    ComponentBox& operator=(const ComponentBox&) = delete;
    ComponentBox(ComponentBox&& other) noexcept
        : type_{other.type_},
          memory_{std::move(other.memory_)},
          pool_{other.pool_},
          slot_{other.slot_},
          component_{other.component_} {
        other.pool_ = nullptr;
        other.component_ = nullptr;
    }
    ComponentBox& operator=(ComponentBox&& other) noexcept {
        if (this != &other) {
            reset();
            type_ = other.type_;
            memory_ = std::move(other.memory_);
            pool_ = other.pool_;
            slot_ = other.slot_;
            component_ = other.component_;
            other.pool_ = nullptr;
            other.component_ = nullptr;
        }
        return *this;
    }

    /// The same component moved into a slot of `pool`, belonging to `owner`.
    /// This box is left empty.
    [[nodiscard]] ComponentBox into_pool(ComponentPool& pool, Node& owner) && {
        ComponentBox moved;
        moved.type_ = type_;
        const ComponentPool::Slot slot = pool.acquire(owner);
        type_->relocate(slot.memory, component_);
        component_ = nullptr;
        free_memory();
        moved.pool_ = &pool;
        moved.slot_ = slot.index;
        moved.component_ = slot.memory;
        pool.admit(slot.index);
        return moved;
    }

    /// The same component moved into memory of its own, out of any pool.
    /// This box is left empty.
    [[nodiscard]] ComponentBox into_own_memory() && {
        ComponentBox moved;
        moved.type_ = type_;
        moved.memory_ = std::make_unique_for_overwrite<std::byte[]>(type_->size);
        type_->relocate(moved.memory_.get(), component_);
        component_ = nullptr;
        free_memory();
        moved.component_ = moved.memory_.get();
        return moved;
    }

    /// False for a default-made box, and for one moved from.
    [[nodiscard]] explicit operator bool() const { return component_ != nullptr; }
    [[nodiscard]] const ComponentType& type() const { return *type_; }
    [[nodiscard]] void* get() const { return component_; }
    /// Whether the component lives in a scene's pool.
    [[nodiscard]] bool pooled() const { return pool_ != nullptr; }

private:
    void reset() {
        if (component_ != nullptr) type_->destroy(component_);
        component_ = nullptr;
        free_memory();
    }

    // Gives the memory back once the component in it has ended or moved out.
    void free_memory() {
        if (pool_ != nullptr) pool_->release(slot_);
        pool_ = nullptr;
        memory_.reset();
    }

    const ComponentType* type_{nullptr};
    std::unique_ptr<std::byte[]> memory_;  // when in memory of its own
    ComponentPool* pool_{nullptr};         // when in a pool
    uint32_t slot_{0};
    void* component_{nullptr};
};

// Type-erased component storage.
// Holds any mix of component types, queryable by compile-time type ID.
// Optimized for small counts (1-4 components per node) via linear scan.
//
// A node in a scene keeps its components in the scene's pools, where the
// systems that want every component of a type find them; a node in no scene
// (an undo snapshot, a preset, one being built) keeps them in memory of their
// own. attach() and detach() move them between the two, and a component keeps
// its address everywhere else.
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
        if (pools_ != nullptr) {
            entries_.push_back(ComponentBox::make_in<T>(pools_->pool(component_type<T>()),
                                                        *owner_, std::forward<Args>(args)...));
        } else {
            entries_.push_back(ComponentBox::make<T>(std::forward<Args>(args)...));
        }
        return *static_cast<T*>(entries_.back().get());
    }

    /// Moves every component into `pools`, for `owner`, the node holding
    /// this storage, which has just joined their scene; components added from
    /// now on go there too.
    void attach(ComponentPools& pools, Node& owner) {
        pools_ = &pools;
        owner_ = &owner;
        for (auto& e : entries_) {
            e = std::move(e).into_pool(pools.pool(e.type()), owner);
        }
    }

    /// Moves every component out of its scene's pools into memory of its
    /// own, for a node leaving the scene.
    void detach() {
        for (auto& e : entries_) {
            e = std::move(e).into_own_memory();
        }
        pools_ = nullptr;
        owner_ = nullptr;
    }

    /// Whether the components live in a scene's pools.
    [[nodiscard]] bool attached() const { return pools_ != nullptr; }

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
                // A detached component is kept apart from any scene: the
                // scene may be gone by the time it is restored.
                out.box_ = entries_[i].pooled() ? std::move(entries_[i]).into_own_memory()
                                                : std::move(entries_[i]);
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
        ComponentBox box = std::move(detached.box_);
        if (pools_ != nullptr) box = std::move(box).into_pool(pools_->pool(box.type()), *owner_);
        entries_.insert(entries_.begin() + static_cast<std::ptrdiff_t>(at), std::move(box));
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
    ComponentPools* pools_{nullptr};  // the scene's, while the node is in one
    Node* owner_{nullptr};            // the node holding this storage, while it is
};

template <typename T>
void clone_component(const ComponentStorage& /*from*/, const T& src, ComponentStorage& to) {
    to.add<T>(src);
}

} // namespace fjell
