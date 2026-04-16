#include "core/asset_id.hpp"

#include <random>

namespace fjell {

static constexpr char HEX_CHARS[] = "0123456789abcdef";

std::string AssetId::to_string() const {
    std::string result;
    result.reserve(32);
    for (auto b : bytes) {
        result += HEX_CHARS[(b >> 4) & 0xF];
        result += HEX_CHARS[b & 0xF];
    }
    return result;
}

AssetId AssetId::from_string(std::string_view hex) {
    AssetId id{};
    if (hex.size() != 32) return id;

    for (std::size_t i = 0; i < 16; ++i) {
        auto hi = hex[i * 2];
        auto lo = hex[i * 2 + 1];

        auto hex_val = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
            if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
            return -1;
        };

        int h = hex_val(hi);
        int l = hex_val(lo);
        if (h < 0 || l < 0) return AssetId{};

        id.bytes[i] = static_cast<uint8_t>((h << 4) | l);
    }
    return id;
}

AssetId AssetId::generate() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};

    AssetId id{};
    // Fill with random bytes (two 64-bit values cover 16 bytes)
    auto r0 = rng();
    auto r1 = rng();
    for (int i = 0; i < 8; ++i) {
        id.bytes[i] = static_cast<uint8_t>(r0 >> (i * 8));
        id.bytes[i + 8] = static_cast<uint8_t>(r1 >> (i * 8));
    }

    // Set UUID v4 version bits: byte 6 = 0100xxxx
    id.bytes[6] = (id.bytes[6] & 0x0F) | 0x40;
    // Set variant bits: byte 8 = 10xxxxxx
    id.bytes[8] = (id.bytes[8] & 0x3F) | 0x80;

    return id;
}

} // namespace fjell
