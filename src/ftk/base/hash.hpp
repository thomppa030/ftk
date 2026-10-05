#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace ftk {

/// Where a 64-bit FNV-1a hash starts.
inline constexpr uint64_t FNV1A_OFFSET = 14695981039346656037ULL;

/// The 64-bit FNV-1a hash of `bytes`, carried on from `seed`, so hashing
/// several pieces in turn is hashing them joined. It is the same on every
/// machine, build and run, which std::hash is free not to be, so it can key
/// what is written to disk and read back by a later build. Cheap, not
/// cryptographic: for telling content apart, not for keeping secrets.
[[nodiscard]] constexpr uint64_t fnv1a64(std::string_view bytes, uint64_t seed = FNV1A_OFFSET) noexcept {
    constexpr uint64_t PRIME = 1099511628211ULL;
    uint64_t h = seed;
    for (const unsigned char b : bytes) {
        h ^= b;
        h *= PRIME;
    }
    return h;
}

/// fnv1a64() over raw bytes.
[[nodiscard]] inline uint64_t fnv1a64(std::span<const std::byte> bytes, uint64_t seed = FNV1A_OFFSET) noexcept {
    return fnv1a64(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), seed);
}

} // namespace ftk
