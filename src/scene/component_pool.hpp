#pragma once

#include "scene/component_type.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace fjell {

struct Node;

/// Every component of one type in a scene, for the systems that want all of
/// them rather than asking node by node.
///
/// Components live in fixed-size chunks that are never reallocated, so a
/// component keeps its address for as long as it lives, however many others
/// come and go. A walk visits them in the order they joined the pool, which
/// depends only on what the scene did, never on where anything happens to
/// sit in memory: two runs that did the same things walk the same components
/// in the same order.
///
/// A component that leaves during a walk is skipped from then on; one that
/// joins during a walk is first visited by the next one.
///
/// Walks keep their bookkeeping in the pool, unguarded: walk it on the thread
/// that owns its scene, and hand workers what the walk found rather than the
/// scene to walk.
class ComponentPool {
public:
    explicit ComponentPool(const ComponentType& type);
    ~ComponentPool();
    ComponentPool(const ComponentPool&) = delete;
    ComponentPool& operator=(const ComponentPool&) = delete;
    ComponentPool(ComponentPool&&) = delete;
    ComponentPool& operator=(ComponentPool&&) = delete;

    [[nodiscard]] const ComponentType& type() const { return *type_; }

    /// Uninitialised memory for a component of this pool's type belonging to
    /// `owner`, and the slot that names it. A node in a scene does not move,
    /// so the pool can hand the owner back to a walk. The caller constructs the
    /// component there and hands it over with admit(); until then walks do
    /// not see it.
    struct Slot {
        uint32_t index;
        void* memory;
    };
    [[nodiscard]] Slot acquire(Node& owner);

    /// Makes the component constructed in `slot` visible to walks.
    void admit(uint32_t slot);

    /// Gives back a slot whose component has already been destroyed (or was
    /// never constructed). Walks stop seeing it at once.
    void release(uint32_t slot);

    /// A walk over the pool: the components that were in it when the walk
    /// began, in join order. One that leaves meanwhile reads as null from then
    /// on; one that joins is not part of it. While any walk is open the pool
    /// keeps every entry where it is.
    class Walk {
    public:
        /// A walk over nothing when `pool` is null.
        explicit Walk(ComponentPool* pool);
        ~Walk();
        Walk(const Walk&) = delete;
        Walk& operator=(const Walk&) = delete;
        Walk(Walk&& other) noexcept : pool_{other.pool_}, count_{other.count_} {
            other.pool_ = nullptr;
        }
        Walk& operator=(Walk&&) = delete;

        [[nodiscard]] std::size_t size() const { return count_; }
        /// The component at `i`, or null where it has left since the walk
        /// began.
        [[nodiscard]] void* component(std::size_t i) const { return pool_->members_[i].component; }
        [[nodiscard]] Node* owner(std::size_t i) const { return pool_->members_[i].owner; }

    private:
        ComponentPool* pool_;
        std::size_t count_{0};
    };

    /// Calls fn(owner, component) for every live component, in join order.
    template <typename Fn>
    void for_each(Fn&& fn) {
        const Walk walk{this};
        for (std::size_t i = 0; i < walk.size(); ++i) {
            if (void* component = walk.component(i)) fn(*walk.owner(i), component);
        }
    }

    /// Live components.
    [[nodiscard]] std::size_t size() const { return members_.size() - released_ - pending_; }

    /// The owner of the live component that joined first, or null when there
    /// is none.
    [[nodiscard]] Node* first_owner() const;

    /// The owner of the live component at `component`, or null when no live
    /// component of this pool is there.
    [[nodiscard]] Node* owner_of(const void* component) const;

private:
    static constexpr uint32_t RELEASED = UINT32_MAX;

    struct Member {
        Node* owner;
        uint32_t slot;     // RELEASED once given back
        void* component;   // null until admitted, and once released
    };

    [[nodiscard]] void* slot_memory(uint32_t slot) const;
    void compact();

    const ComponentType* type_;
    uint32_t stride_;
    uint32_t slots_per_chunk_;
    std::vector<std::unique_ptr<std::byte[]>> chunks_;
    uint32_t slot_count_{0};              // slots ever handed out, across all chunks
    std::vector<uint32_t> free_slots_;
    std::vector<Member> members_;         // walk order
    std::vector<uint32_t> member_of_;     // slot -> index in members_
    std::size_t released_{0};             // members_ entries given back, until compacted
    std::size_t pending_{0};              // acquired and not yet admitted
    int walks_{0};                        // walks in progress
};

/// The components of one type in a scene as a range, for
/// `for (auto [node, mesh] : scene.view<MeshComponent>())`: each step gives the
/// node holding a component and the component, in join order, under a pool
/// walk's rules. `NodeT` and `T` are const for a const scene.
template <typename NodeT, typename T>
class ComponentView {
public:
    struct Entry {
        NodeT* node;
        T* component;
    };

    class iterator {
    public:
        iterator(const ComponentPool::Walk* walk, std::size_t index) : walk_{walk}, index_{index} {
            skip_gone();
        }
        [[nodiscard]] Entry operator*() const {
            return {walk_->owner(index_), static_cast<T*>(walk_->component(index_))};
        }
        iterator& operator++() {
            ++index_;
            skip_gone();
            return *this;
        }
        [[nodiscard]] bool operator==(const iterator& other) const { return index_ == other.index_; }

    private:
        void skip_gone() {
            while (index_ < walk_->size() && walk_->component(index_) == nullptr) ++index_;
        }
        const ComponentPool::Walk* walk_;
        std::size_t index_;
    };

    /// Over nothing when `pool` is null.
    explicit ComponentView(ComponentPool* pool) : walk_{pool} {}

    [[nodiscard]] iterator begin() const { return {&walk_, 0}; }
    [[nodiscard]] iterator end() const { return {&walk_, walk_.size()}; }
    [[nodiscard]] bool empty() const { return begin() == end(); }

private:
    ComponentPool::Walk walk_;
};

/// A scene's pools, one per component type, each made on first use. Owned
/// through a pointer so its address survives the scene being moved: every
/// component of the scene's nodes refers back to it.
class ComponentPools {
public:
    [[nodiscard]] ComponentPool& pool(const ComponentType& type);

    /// The pool for `id`, or null when no component of that type ever joined.
    [[nodiscard]] ComponentPool* find(ComponentTypeId id) const;

    /// Forgets the pool for `id`, which must be empty. For a type that is
    /// about to stop existing: a script type when the game library unloads,
    /// whose next build may lay the type out differently.
    void drop(ComponentTypeId id);

private:
    std::vector<std::unique_ptr<ComponentPool>> by_id_;
};

} // namespace fjell
