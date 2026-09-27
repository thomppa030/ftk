#include "scene/component_pool.hpp"
#include "scene/removal_epoch.hpp"

#include <algorithm>
#include <cassert>

namespace fjell {

namespace {

// Aim for chunks of about this many bytes; a component larger than that gets
// a chunk of its own.
constexpr uint32_t CHUNK_BYTES = 16 * 1024;

uint32_t stride_of(const ComponentType& type) {
    const uint32_t align = type.align;
    return (type.size + align - 1) / align * align;
}

} // namespace

ComponentPool::ComponentPool(const ComponentType& type)
    : type_{&type},
      stride_{stride_of(type)},
      slots_per_chunk_{std::max<uint32_t>(1, CHUNK_BYTES / stride_of(type))} {}

ComponentPool::~ComponentPool() {
    // Every component is destroyed by the box that holds it, and the scene
    // releases its nodes before its pools.
    assert(size() == 0 && pending_ == 0 && "a component outlived the pool it lives in");
}

void* ComponentPool::slot_memory(uint32_t slot) const {
    const uint32_t chunk = slot / slots_per_chunk_;
    const uint32_t offset = (slot % slots_per_chunk_) * stride_;
    return chunks_[chunk].get() + offset;
}

ComponentPool::Slot ComponentPool::acquire(Node& owner) {
    uint32_t slot = 0;
    if (!free_slots_.empty()) {
        slot = free_slots_.back();
        free_slots_.pop_back();
    } else {
        slot = slot_count_++;
        if (slot / slots_per_chunk_ >= chunks_.size()) {
            chunks_.push_back(std::make_unique_for_overwrite<std::byte[]>(
                std::size_t{slots_per_chunk_} * stride_));
        }
        member_of_.push_back(0);
    }
    member_of_[slot] = static_cast<uint32_t>(members_.size());
    members_.push_back({&owner, slot, nullptr});
    ++pending_;
    return {slot, slot_memory(slot)};
}

void ComponentPool::admit(uint32_t slot) {
    Member& m = members_[member_of_[slot]];
    assert(m.component == nullptr && m.slot == slot);
    m.component = slot_memory(slot);
    --pending_;
}

Node* ComponentPool::first_owner() const {
    for (const Member& m : members_) {
        if (m.component != nullptr) return m.owner;
    }
    return nullptr;
}

void ComponentPool::release(uint32_t slot) {
    note_removal();
    Member& m = members_[member_of_[slot]];
    assert(m.slot == slot);
    if (m.component == nullptr) --pending_;
    m.component = nullptr;
    m.slot = RELEASED;
    ++released_;
    free_slots_.push_back(slot);
    // Compacting now would pull entries a walk has not reached yet into
    // places it has passed; wait for the walk to end.
    if (walks_ == 0 && released_ * 2 > members_.size()) compact();
}

void ComponentPool::compact() {
    // Stable, so what is left keeps its walk order.
    std::size_t kept = 0;
    for (const Member& m : members_) {
        if (m.slot == RELEASED) continue;
        member_of_[m.slot] = static_cast<uint32_t>(kept);
        members_[kept++] = m;
    }
    members_.resize(kept);
    released_ = 0;
}

ComponentPool::Walk::Walk(ComponentPool* pool) : pool_{pool} {
    if (pool_ == nullptr) return;
    if (pool_->walks_ == 0 && pool_->released_ > 0) pool_->compact();
    ++pool_->walks_;
    count_ = pool_->members_.size();
}

ComponentPool::Walk::~Walk() {
    if (pool_ != nullptr) --pool_->walks_;
}

ComponentPool& ComponentPools::pool(const ComponentType& type) {
    if (type.id >= by_id_.size()) by_id_.resize(type.id + 1);
    auto& slot = by_id_[type.id];
    if (!slot) slot = std::make_unique<ComponentPool>(type);
    return *slot;
}

void ComponentPools::drop(ComponentTypeId id) {
    if (id < by_id_.size()) by_id_[id].reset();
}

ComponentPool* ComponentPools::find(ComponentTypeId id) const {
    return id < by_id_.size() ? by_id_[id].get() : nullptr;
}

} // namespace fjell
