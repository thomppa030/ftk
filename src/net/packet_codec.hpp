// packet_codec.hpp — Wire-format primitives for the networking layer.
//
// Pure, stateless serialization helpers: append trivially-copyable values and
// length-prefixed strings to a byte buffer, and read them back from a span with
// truncation checks. No dependency on Scene, Transport, or any net state — this
// is the only place that knows the byte layout, so it's unit-testable in
// isolation and the single point to change for wire versioning/compression.
#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace fjell::netcodec {

// Append a trivially-copyable value's raw bytes to a buffer.
template <typename T>
void put(std::vector<uint8_t>& buf, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    const auto* p = reinterpret_cast<const uint8_t*>(&value);
    buf.insert(buf.end(), p, p + sizeof(T));
}

inline void put_bytes(std::vector<uint8_t>& buf, const uint8_t* data, size_t n) {
    buf.insert(buf.end(), data, data + n);
}

// Read a trivially-copyable value from a span, advancing it. Returns false if
// the span is too short (truncated/malformed packet).
template <typename T>
[[nodiscard]] bool take(std::span<const uint8_t>& sp, T& out) {
    static_assert(std::is_trivially_copyable_v<T>);
    if (sp.size() < sizeof(T)) return false;
    std::memcpy(&out, sp.data(), sizeof(T));
    sp = sp.subspan(sizeof(T));
    return true;
}

// Length-prefixed string (uint16 length + bytes).
inline void put_string(std::vector<uint8_t>& buf, const std::string& s) {
    put(buf, static_cast<uint16_t>(s.size()));
    put_bytes(buf, reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

[[nodiscard]] inline bool take_string(std::span<const uint8_t>& sp, std::string& out) {
    uint16_t len = 0;
    if (!take(sp, len)) return false;
    if (sp.size() < len) return false;
    out.assign(reinterpret_cast<const char*>(sp.data()), len);
    sp = sp.subspan(len);
    return true;
}

} // namespace fjell::netcodec
