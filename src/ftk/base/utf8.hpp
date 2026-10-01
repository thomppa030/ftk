#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace fjell::utf8 {

/// What a byte sequence that is not valid UTF-8 decodes to.
inline constexpr uint32_t REPLACEMENT = 0xFFFD;

/// Decodes the codepoint that starts at `text[i]` and moves `i` past it.
///
/// Anything that is not well-formed UTF-8 decodes as REPLACEMENT and moves
/// `i` on by one byte: a stray continuation byte, a sequence cut short, an
/// overlong form, a surrogate, or a value past U+10FFFF. Decoding therefore
/// always makes progress and picks up again at the next lead byte, so one bad
/// byte costs one character, not the rest of the string.
///
/// @param text The UTF-8 text.
/// @param i    Byte offset of the codepoint; must be less than `text.size()`.
/// @return The codepoint, or REPLACEMENT.
[[nodiscard]] constexpr uint32_t next(std::string_view text, size_t& i) {
    auto byte = [&](size_t at) { return static_cast<uint8_t>(text[at]); };
    const uint8_t lead = byte(i);

    if (lead < 0x80) {
        ++i;
        return lead;
    }

    size_t length = 0;
    uint32_t cp = 0;
    uint32_t smallest = 0; // below this the same value had a shorter form
    if ((lead & 0xE0) == 0xC0) {
        length = 2;
        cp = lead & 0x1Fu;
        smallest = 0x80;
    } else if ((lead & 0xF0) == 0xE0) {
        length = 3;
        cp = lead & 0x0Fu;
        smallest = 0x800;
    } else if ((lead & 0xF8) == 0xF0) {
        length = 4;
        cp = lead & 0x07u;
        smallest = 0x10000;
    } else {
        ++i; // a continuation byte, or a lead byte UTF-8 never uses
        return REPLACEMENT;
    }

    if (i + length > text.size()) {
        ++i;
        return REPLACEMENT;
    }
    for (size_t k = 1; k < length; ++k) {
        const uint8_t b = byte(i + k);
        if ((b & 0xC0) != 0x80) {
            ++i;
            return REPLACEMENT;
        }
        cp = (cp << 6) | (b & 0x3Fu);
    }

    if (cp < smallest || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        ++i;
        return REPLACEMENT;
    }
    i += length;
    return cp;
}

} // namespace fjell::utf8
