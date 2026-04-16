#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace fjell {

/// 128-bit UUID v4 identifier for assets.
/// Stored as 32 hex characters (no dashes) in JSON.
struct AssetId {
    std::array<uint8_t, 16> bytes{};

    [[nodiscard]] bool valid() const {
        for (auto b : bytes) {
            if (b != 0) return true;
        }
        return false;
    }

    /// 32 hex characters, lowercase, no dashes.
    [[nodiscard]] std::string to_string() const;

    /// Parse from 32 hex characters. Returns invalid AssetId on bad input.
    static AssetId from_string(std::string_view hex);

    /// Generate a random UUID v4.
    static AssetId generate();

    bool operator==(const AssetId&) const = default;
    auto operator<=>(const AssetId&) const = default;
};

} // namespace fjell

template <>
struct std::hash<fjell::AssetId> {
    std::size_t operator()(const fjell::AssetId& id) const noexcept {
        // FNV-1a over 16 bytes
        std::size_t h = 14695981039346656037ULL;
        for (auto b : id.bytes) {
            h ^= static_cast<std::size_t>(b);
            h *= 1099511628211ULL;
        }
        return h;
    }
};
