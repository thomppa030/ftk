#include "ftk/base/utf8.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace ftk;

static std::vector<uint32_t> decode(std::string_view text) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < text.size();) out.push_back(utf8::next(text, i));
    return out;
}

TEST_CASE("ASCII decodes byte for byte", "[utf8]") {
    REQUIRE(decode("Hi 7") == std::vector<uint32_t>{'H', 'i', ' ', '7'});
}

TEST_CASE("Two, three and four byte sequences decode to one codepoint", "[utf8]") {
    REQUIRE(decode("\xC3\x97") == std::vector<uint32_t>{0xD7});         // multiplication sign
    REQUIRE(decode("\xC3\xB6") == std::vector<uint32_t>{0xF6});         // o with diaeresis
    REQUIRE(decode("\xE2\x80\x93") == std::vector<uint32_t>{0x2013});   // en dash
    REQUIRE(decode("\xE2\x82\xAC") == std::vector<uint32_t>{0x20AC});   // euro sign
    REQUIRE(decode("\xF0\x9F\x90\x9F") == std::vector<uint32_t>{0x1F41F});
    REQUIRE(decode("\xC3\x97" "24") == std::vector<uint32_t>{0xD7, '2', '4'});
}

TEST_CASE("The largest codepoint of each length decodes", "[utf8]") {
    REQUIRE(decode("\xDF\xBF") == std::vector<uint32_t>{0x7FF});
    REQUIRE(decode("\xEF\xBF\xBF") == std::vector<uint32_t>{0xFFFF});
    REQUIRE(decode("\xF4\x8F\xBF\xBF") == std::vector<uint32_t>{0x10FFFF});
}

TEST_CASE("A stray continuation byte costs one character", "[utf8]") {
    REQUIRE(decode("a\x97" "b") == std::vector<uint32_t>{'a', utf8::REPLACEMENT, 'b'});
}

TEST_CASE("A sequence cut short resynchronises at the next byte", "[utf8]") {
    // The lead byte promises two more; 'x' is not a continuation.
    REQUIRE(decode("\xE2\x80x") == std::vector<uint32_t>{utf8::REPLACEMENT, utf8::REPLACEMENT, 'x'});
    // Truncated by the end of the text.
    REQUIRE(decode("ok\xC3") == std::vector<uint32_t>{'o', 'k', utf8::REPLACEMENT});
}

TEST_CASE("Overlong forms, surrogates and values past U+10FFFF are refused", "[utf8]") {
    REQUIRE(decode("\xC0\x80").front() == utf8::REPLACEMENT);          // overlong NUL
    REQUIRE(decode("\xE0\x80\xAF").front() == utf8::REPLACEMENT);      // overlong '/'
    REQUIRE(decode("\xED\xA0\x80").front() == utf8::REPLACEMENT);      // U+D800
    REQUIRE(decode("\xF4\x90\x80\x80").front() == utf8::REPLACEMENT);  // U+110000
    REQUIRE(decode("\xFF").front() == utf8::REPLACEMENT);
}

TEST_CASE("Decoding always moves forward", "[utf8]") {
    // Every byte value as a lead, alone and followed by continuations: the
    // decoder must never stall or run past the end.
    for (int b = 0; b < 256; ++b) {
        for (const char* tail : {"", "\x80", "\x80\x80", "\x80\x80\x80"}) {
            std::string text(1, static_cast<char>(b));
            text += tail;
            size_t i = 0;
            size_t steps = 0;
            while (i < text.size()) {
                size_t before = i;
                (void)utf8::next(text, i);
                REQUIRE(i > before);
                REQUIRE(i <= text.size());
                ++steps;
            }
            REQUIRE(steps <= text.size());
        }
    }
}
