#pragma once

#include <cctype>
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

} // namespace ftk
