#include "ftk/ui/kit/color_code.hpp"

#include <algorithm>
#include <cstdio>

namespace ftk::ui::detail {

namespace {

int to_byte(float v) {
    return static_cast<int>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

} // namespace

std::string to_hex(const float* rgba, bool alpha) {
    char hex[16];
    if (alpha) {
        std::snprintf(hex, sizeof(hex), "#%02X%02X%02X%02X", to_byte(rgba[0]), to_byte(rgba[1]),
                      to_byte(rgba[2]), to_byte(rgba[3]));
    } else {
        std::snprintf(hex, sizeof(hex), "#%02X%02X%02X", to_byte(rgba[0]), to_byte(rgba[1]),
                      to_byte(rgba[2]));
    }
    return hex;
}

bool from_hex(const std::string& text, float* rgba, bool alpha) {
    const std::size_t start = !text.empty() && text[0] == '#' ? 1 : 0;
    const std::size_t digits = text.size() - start;
    if (digits != 6 && !(alpha && digits == 8)) return false;
    float parsed[4]{0.0f, 0.0f, 0.0f, 1.0f};
    for (std::size_t i = 0; i < digits / 2; ++i) {
        unsigned byte = 0;
        const std::string pair = text.substr(start + i * 2, 2);
        if (pair.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return false;
        if (std::sscanf(pair.c_str(), "%2x", &byte) != 1) return false;
        parsed[i] = static_cast<float>(byte) / 255.0f;
    }
    const std::size_t count = alpha ? 4 : 3;
    if (alpha && digits == 6) parsed[3] = rgba[3];
    std::copy(parsed, parsed + count, rgba);
    return true;
}

} // namespace ftk::ui::detail
