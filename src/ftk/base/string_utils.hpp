#pragma once

#include <array>
#include <cctype>
#include <cstdint>
#include <format>
#include <string>

namespace ftk {

/// Convert "velocity_min" → "Velocity Min"
inline std::string display_name(const std::string& snake) {
    std::string out;
    out.reserve(snake.size());
    bool cap_next = true;
    for (char c : snake) {
        if (c == '_') {
            out += ' ';
            cap_next = true;
        } else if (cap_next) {
            out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            cap_next = false;
        } else {
            out += c;
        }
    }
    return out;
}

/// A size in bytes as a person reads it: "512 B", then "5.3 KiB", "1.2 MiB",
/// "2.0 GiB", each unit 1024 of the one before.
inline std::string size_text(std::uint64_t bytes) {
    if (bytes < 1024) return std::format("{} B", bytes);
    constexpr std::array<const char*, 4> UNITS{"KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes) / 1024.0;
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < UNITS.size()) {
        value /= 1024.0;
        ++unit;
    }
    return std::format("{:.1f} {}", value, UNITS[unit]);
}

} // namespace ftk
