#pragma once

#include "core/handle.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace fjell {

/// Objects of one kind, each found by a generational handle.
///
/// Removing an object frees its slot for the next one under a new generation,
/// so a handle kept past the removal finds nothing rather than the newcomer.
/// Slots live in fixed-size chunks that never move once allocated: a pointer
/// from `get()` stays valid until that object is removed, however many are
/// added meanwhile. Adding and removing belong to one thread; any thread may
/// look up a handle that was made before it was handed the handle, while that
/// thread adds.
///
/// @code
/// HandlePool<Image, ImageTag> images;
/// Handle<ImageTag> h = images.emplace(width, height);
/// if (Image* image = images.get(h)) { ... }
/// std::optional<Image> gone = images.take(h);   // images.get(h) is null from here
/// @endcode
template <typename T, typename Tag>
class HandlePool {
public:
    using HandleT = Handle<Tag>;

    /// Constructs an object in a free slot and returns its handle.
    template <typename... Args>
    HandleT emplace(Args&&... args) {
        uint32_t index = 0;
        if (!free_.empty()) {
            index = free_.back();
            free_.pop_back();
        } else {
            index = slots_used_.load(std::memory_order_relaxed);
            assert(index < MAX_SLOTS && "HandlePool: a handle's index has 20 bits");
            if (chunks_[index / CHUNK_SIZE] == nullptr) {
                chunks_[index / CHUNK_SIZE] = std::make_unique<Chunk>();
            }
            slots_used_.store(index + 1, std::memory_order_release);
        }
        Slot& slot = slot_at(index);
        slot.value.emplace(std::forward<Args>(args)...);
        ++count_;
        return HandleT::make(index, slot.generation);
    }

    /// The object, or null when the handle is invalid, was removed, or is not
    /// this pool's.
    [[nodiscard]] const T* get(HandleT handle) const {
        const Slot* slot = find(handle);
        return slot != nullptr ? &*slot->value : nullptr;
    }
    [[nodiscard]] T* get(HandleT handle) {
        return const_cast<T*>(std::as_const(*this).get(handle));
    }

    /// Whether the handle finds an object.
    [[nodiscard]] bool contains(HandleT handle) const { return find(handle) != nullptr; }

    /// Removes the object and hands it back, for whoever destroys it later.
    /// Returns nothing when the handle finds no object.
    std::optional<T> take(HandleT handle) {
        if (find(handle) == nullptr) return std::nullopt;
        Slot& slot = slot_at(handle.index());
        std::optional<T> out = std::move(slot.value);
        slot.value.reset();
        // Generation 0 is skipped on wrap, so slot 0 never hands out id 0.
        slot.generation = (slot.generation + 1) & MAX_GENERATION;
        if (slot.generation == 0) slot.generation = 1;
        free_.push_back(handle.index());
        --count_;
        return out;
    }

    /// Objects held.
    [[nodiscard]] uint32_t size() const { return count_; }

private:
    static constexpr uint32_t CHUNK_SIZE = 256;
    static constexpr uint32_t MAX_SLOTS = 1u << 20;            // Handle's index bits
    static constexpr uint32_t MAX_GENERATION = (1u << 12) - 1; // Handle's generation bits

    struct Slot {
        std::optional<T> value;
        uint32_t generation{1};
    };
    using Chunk = std::array<Slot, CHUNK_SIZE>;

    [[nodiscard]] Slot& slot_at(uint32_t index) {
        return (*chunks_[index / CHUNK_SIZE])[index % CHUNK_SIZE];
    }
    [[nodiscard]] const Slot& slot_at(uint32_t index) const {
        return (*chunks_[index / CHUNK_SIZE])[index % CHUNK_SIZE];
    }

    [[nodiscard]] const Slot* find(HandleT handle) const {
        if (!handle.valid() || handle.index() >= slots_used_.load(std::memory_order_acquire)) {
            return nullptr;
        }
        const Slot& slot = slot_at(handle.index());
        if (!slot.value.has_value() || slot.generation != handle.generation()) return nullptr;
        return &slot;
    }

    // A fixed table, so adding a chunk never moves the pointers to the others.
    std::array<std::unique_ptr<Chunk>, MAX_SLOTS / CHUNK_SIZE> chunks_{};
    std::vector<uint32_t> free_;
    std::atomic<uint32_t> slots_used_{0};
    uint32_t count_{0};
};

} // namespace fjell
