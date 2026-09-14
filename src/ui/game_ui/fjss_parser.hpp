#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
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

struct StyleRule {
    StyleSelector selector;
    std::unordered_map<std::string, std::string> properties;
};

struct Stylesheet {
    std::vector<StyleRule> rules;
};

/// Parser for .fjss (CSS-like) stylesheets.
class FjssParser {
public:
    [[nodiscard]] static Stylesheet parse(std::string_view source);

private:
    explicit FjssParser(std::string_view source);

    Stylesheet parse_stylesheet();
    /// One declaration block may be shared by several comma-separated
    /// selectors (`.a, .b { ... }`), so a single source rule can yield
    /// several StyleRules.
    std::vector<StyleRule> parse_rule();
    StyleSelector parse_selector();
    void parse_declarations(std::unordered_map<std::string, std::string>& props);

    void skip_whitespace();
    void skip_comment();
    /// Error recovery: consume through the end of the current construct
    /// (past the next `;` or a balanced `}`) so the parser can carry on
    /// with the following rule instead of stalling on the bad one.
    void skip_malformed_rule();
    [[nodiscard]] bool at_end() const;
    [[nodiscard]] char peek() const;
    char advance();

    std::string_view source_;
    size_t pos_{0};
};

} // namespace fjell
