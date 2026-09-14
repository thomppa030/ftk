#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fjell {

struct StyleSelector {
    std::string tag;      // e.g. "div"
    std::string id;       // e.g. "health-bar" (without #)
    std::string class_;   // e.g. "panel" (without .)
    std::string parent;   // ancestor tag for descendant combinator
    std::string pseudo;   // e.g. "hover", "active", "focus" (without :)

    [[nodiscard]] uint32_t specificity() const {
        uint32_t s = 0;
        if (!id.empty()) s += 100;
        if (!class_.empty()) s += 10;
        if (!pseudo.empty()) s += 10;
        if (!tag.empty()) s += 1;
        return s;
    }
};

/// One authored `name: value;`. Declarations keep their source order so a
/// shorthand and a longhand in the same rule apply the way they were
/// written (`margin: 10; margin-left: 20;` must leave the left edge at 20).
struct StyleDeclaration {
    std::string name;
    std::string value;
    /// `!important` was written. Kept so the sheet round-trips through the
    /// serializer; the cascade does not weigh it yet.
    bool important{false};
};

struct StyleRule {
    StyleSelector selector;
    std::vector<StyleDeclaration> declarations;

    /// The declaration that wins for `name` within this rule — the last one
    /// written, as in CSS — or nullptr when the rule does not set it.
    [[nodiscard]] const StyleDeclaration* find(std::string_view name) const;
    [[nodiscard]] StyleDeclaration* find(std::string_view name);
    /// Overwrites the winning declaration in place, keeping its authored
    /// position, or appends one when the rule did not set `name` yet.
    void set(std::string_view name, std::string value);
    void erase(std::string_view name);
};

enum class FjssSeverity : uint8_t {
    warning, ///< a declaration or rule was dropped, the rest of the sheet applies
    error,   ///< the sheet's structure is broken from this point on
};

/// A problem found while parsing, positioned so an editor can point at the
/// offending text and a log line can name file:line:column.
struct FjssDiagnostic {
    std::string source; ///< file the text came from; empty for in-memory text
    uint32_t line{1};   ///< 1-based
    uint32_t column{1}; ///< 1-based, in bytes
    FjssSeverity severity{FjssSeverity::warning};
    std::string message;
};

struct Stylesheet {
    std::vector<StyleRule> rules;
    /// Problems from this sheet and from every widget sheet merged into it.
    std::vector<FjssDiagnostic> diagnostics;
};

/// Parser for .fjss (CSS-like) stylesheets.
///
/// Malformed input never stops the parse: the offending construct is
/// skipped, recorded as a diagnostic, and parsing resumes at the next rule.
class FjssParser {
public:
    /// source_name is carried on every diagnostic; pass the file path when
    /// the text was read from disk.
    [[nodiscard]] static Stylesheet parse(std::string_view source,
                                          std::string_view source_name = {});

private:
    FjssParser(std::string_view source, std::string_view source_name);

    Stylesheet parse_stylesheet();
    /// One declaration block may be shared by several comma-separated
    /// selectors (`.a, .b { ... }`), so a single source rule can yield
    /// several StyleRules.
    std::vector<StyleRule> parse_rule();
    StyleSelector parse_selector();
    void parse_declarations(std::vector<StyleDeclaration>& declarations);

    void skip_whitespace();
    void skip_comment();
    /// Error recovery: consume through the end of the current construct
    /// (past the next `;` or a balanced `}`) so the parser can carry on
    /// with the following rule instead of stalling on the bad one.
    void skip_malformed_rule();
    /// Consumes a `{ ... }` block including nested ones, starting at the `{`.
    void skip_block();
    /// At-rules (`@import`, `@media`, `@keyframes`, ...) are not supported.
    /// The whole construct is consumed and reported so the rules after it
    /// still parse.
    void skip_at_rule();
    [[nodiscard]] bool at_end() const;
    [[nodiscard]] char peek() const;
    char advance();

    struct SourcePosition {
        uint32_t line;
        uint32_t column;
    };
    /// Line and column of a byte offset, counted on demand. Diagnostics are
    /// rare enough that a scan per report beats threading counters through
    /// every advance().
    [[nodiscard]] SourcePosition position_at(size_t offset) const;
    void diagnose(size_t offset, FjssSeverity severity, std::string message);
    void log_diagnostics() const;

    std::string_view source_;
    std::string source_name_;
    size_t pos_{0};
    Stylesheet sheet_;
    /// Set by parse_selector on syntax the engine cannot match (`::before`,
    /// `:not(...)`, `>`, `[attr]`, `*`). The rule is still consumed so the
    /// sheet stays in sync, then dropped with this as the reason.
    std::string selector_problem_;
};

} // namespace fjell
