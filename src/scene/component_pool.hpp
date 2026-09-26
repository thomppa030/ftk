#pragma once

#include "scene/component_type.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace fjell {

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
    /// `node_id`, and the slot that names it. The caller constructs the
    /// component there and hands it over with admit(); until then walks do
    /// not see it.
    struct Slot {
        uint32_t index;
        void* memory;
    };
    [[nodiscard]] Slot acquire(uint32_t node_id);

    /// Makes the component constructed in `slot` visible to walks.
    void admit(uint32_t slot);

    /// Gives back a slot whose component has already been destroyed (or was
    /// never constructed). Walks stop seeing it at once.
    void release(uint32_t slot);

    /// Calls fn(node_id, component) for every live component, in join order.
    template <typename Fn>
    void for_each(Fn&& fn) {
        WalkGuard guard{*this};
        const std::size_t count = members_.size();
        for (std::size_t i = 0; i < count; ++i) {
            const Member& m = members_[i];
            if (m.component != nullptr) fn(m.node_id, m.component);
        }
    }

    /// Live components.
    [[nodiscard]] std::size_t size() const { return members_.size() - released_ - pending_; }

private:
    static constexpr uint32_t RELEASED = UINT32_MAX;

    struct Member {
        uint32_t node_id;
        uint32_t slot;     // RELEASED once given back
        void* component;   // null until admitted, and once released
    };

    struct WalkGuard {
        explicit WalkGuard(ComponentPool& pool);
        ~WalkGuard();
        WalkGuard(const WalkGuard&) = delete;
        WalkGuard& operator=(const WalkGuard&) = delete;
        ComponentPool& pool;
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

/// A scene's pools, one per component type, each made on first use. Owned
/// through a pointer so its address survives the scene being moved: every
/// component of the scene's nodes refers back to it.
class ComponentPools {
public:
    [[nodiscard]] ComponentPool& pool(const ComponentType& type);

    /// The pool for `id`, or null when no component of that type ever joined.
    [[nodiscard]] ComponentPool* find(ComponentTypeId id) const;

private:
    std::vector<std::unique_ptr<ComponentPool>> by_id_;
};

} // namespace fjell
