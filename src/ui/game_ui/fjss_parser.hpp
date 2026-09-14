#pragma once

#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fjell {

/// How an ancestor compound relates to the compound on its right.
enum class UiCombinator : uint8_t {
    descendant, ///< `a b`   — any ancestor, however deep
    child,      ///< `a > b` — the immediate parent only
};

/// One compound selector: everything between two combinators, e.g. the
/// `div#main.panel.wide:hover` of `body > div#main.panel.wide:hover`.
/// Every listed part must hold on the same node.
struct StyleCompound {
    std::string tag;                        // e.g. "div"
    std::string id;                         // e.g. "health-bar" (without #)
    std::vector<std::string> classes;       // e.g. {"panel", "wide"} (without .)
    std::vector<std::string> pseudo_classes; // e.g. {"hover"} (without :)

    [[nodiscard]] bool empty() const {
        return tag.empty() && id.empty() && classes.empty() && pseudo_classes.empty();
    }
};

/// One ancestor step of a selector chain, with the combinator that joins
/// it to the compound on its right.
struct StyleAncestorStep {
    StyleCompound compound;
    UiCombinator combinator{UiCombinator::descendant};
};

/// A full selector: the subject compound plus the ancestors that must match
/// above it, stored outermost first so it reads in source order
/// (`a > b c` is ancestors [a via child, b via descendant], subject `c`).
struct StyleSelector {
    StyleCompound subject;
    std::vector<StyleAncestorStep> ancestors;

    /// CSS specificity — (ids, classes and pseudo-classes, tags) counted
    /// across every compound — packed one byte per field so a single integer
    /// comparison orders selectors the way the spec's triple does.
    [[nodiscard]] uint32_t specificity() const;
};

/// The selector as it is written in a stylesheet, e.g. `panel > .slot:hover`.
[[nodiscard]] std::string to_css(const StyleSelector& selector);

/// True when the selector is exactly `#id` with nothing else: no tag, class,
/// pseudo-class or ancestor. The editor routes its per-node style edits into
/// such a rule, and every place that needs to recognize one must agree.
[[nodiscard]] bool selects_only_id(const StyleSelector& selector, std::string_view id);

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
    /// The file the rule was read from, or that an editor-made rule will be
    /// written to. A loaded sheet holds rules from several files — the root
    /// sheet and every widget sheet merged into it — so a save has to know
    /// which rules belong to the file being written.
    std::string source;

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
    /// A chain of compounds joined by combinators, up to `{` or `,`.
    StyleSelector parse_selector();
    /// One part of a compound — `tag`, `.class`, `#id` or `:pseudo` — appended
    /// to `compound`. Always consumes at least one character.
    void parse_compound_part(StyleCompound& compound);
    void parse_declarations(std::vector<StyleDeclaration>& declarations);
    /// Reports a property the engine does not know, or a keyword value the
    /// property does not accept.
    void validate_declaration(size_t offset, const std::string& name, const std::string& value);
    /// Each token must be a number in px or %, or `auto` where the property
    /// takes it. Other units have no meaning to the layout engine yet.
    void validate_lengths(size_t offset, const struct UiPropertyDescriptor& desc,
                          const std::string& value);

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
    [[nodiscard]] std::string read_identifier();

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
    /// Records the first reason a selector cannot be matched (`::before`,
    /// `:not(...)`, `[attr]`, `*`, `+`, `~`). The rule is still consumed so
    /// the sheet stays in sync, then dropped with this as the reason.
    void reject_selector(std::string reason);

    std::string_view source_;
    std::string source_name_;
    size_t pos_{0};
    Stylesheet sheet_;
    std::string selector_problem_;
};

} // namespace fjell
