#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace ftk {

/// Generational handle: a slot index and the generation the slot had when the
/// handle was made, so a handle kept after its object was removed no longer
/// matches the slot's next occupant. The tag only keeps handles of different
/// kinds apart. Id 0 is the invalid handle.
/// Bits [0..19]  = slot index  (max 1M entries)
/// Bits [20..31] = generation  (wraps after 4096)
template <typename Tag>
struct Handle {
    uint32_t id{0};

    [[nodiscard]] bool valid() const { return id != 0; }
    [[nodiscard]] uint32_t index() const { return id & 0xFFFFF; }
    [[nodiscard]] uint32_t generation() const { return id >> 20; }

    static Handle make(uint32_t index, uint32_t gen) {
        return {(gen << 20) | (index & 0xFFFFF)};
    }

    bool operator==(const Handle&) const = default;
};

/// Hashes a handle by its id, for maps keyed by handles.
struct HandleHash {
    template <typename Tag>
    [[nodiscard]] size_t operator()(Handle<Tag> handle) const noexcept {
        return std::hash<uint32_t>{}(handle.id);
    }
};

} // namespace ftk
