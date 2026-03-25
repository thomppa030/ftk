#pragma once

#include <imgui.h>

#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace fjell {

enum class TokenType {
    Default,
    Keyword,
    Type,
    Builtin,       // built-in variables/functions (ALBEDO, gl_Position, etc.)
    String,
    Number,
    Comment,
    Preprocessor,
    Punctuation,
    Attribute,     // HTML attributes, CSS properties
    Tag,           // HTML/XML tags
    Binding,       // {data} template bindings in FJUI
    Selector,      // CSS selectors (.class, #id, :pseudo)
};

struct Token {
    int start;      // column (byte offset in line)
    int end;        // column end (exclusive)
    TokenType type;
};

// Colors for each token type (designed for dark theme, #1e1f22 background)
[[nodiscard]] ImU32 token_color(TokenType type);

enum class Language {
    PlainText,
    Cpp,
    Glsl,
    Fjsl,
    Fjui,
    Fjss,
    Json,
};

// Detect language from file extension
[[nodiscard]] Language detect_language(std::string_view path);

// Per-line tokenizer state (for multi-line comments)
struct LineState {
    bool in_block_comment{false};
};

// Tokenize a single line, given the state from the previous line.
// Returns the state for the next line.
[[nodiscard]] LineState tokenize_line(
    std::string_view line,
    Language lang,
    LineState prev_state,
    std::vector<Token>& out_tokens);

} // namespace fjell
