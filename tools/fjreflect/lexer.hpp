#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace fjreflect {

enum class TokenKind {
    identifier, // names and keywords alike: the parser tells them apart
    number,
    string,     // a string literal, prefixes, quotes and suffix included
    character,  // a character literal, quotes included
    punct,      // one character, or one of :: -> ...
    end,        // one past the last token
};

struct Token {
    TokenKind kind{TokenKind::end};
    std::string text;
    unsigned line{0}; // 1-based
};

struct LexResult {
    std::vector<Token> tokens; // always ends with a TokenKind::end token
    std::vector<std::string> errors; // "line: message", without the file name
};

/// Split C++ source into tokens. Comments and preprocessor directives are
/// dropped; string, character and raw string literals come out whole, so
/// nothing inside one can pass for code. `>>` stays two `>` tokens, which is
/// what closing two template argument lists needs.
[[nodiscard]] LexResult tokenize(std::string_view source);

} // namespace fjreflect
