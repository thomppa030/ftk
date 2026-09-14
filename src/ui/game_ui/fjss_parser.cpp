#include "ui/game_ui/fjss_parser.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <cctype>

namespace fjell {

FjssParser::FjssParser(std::string_view source, std::string_view source_name)
    : source_{source}, source_name_{source_name} {}

Stylesheet FjssParser::parse(std::string_view source, std::string_view source_name) {
    FjssParser parser{source, source_name};
    return parser.parse_stylesheet();
}

Stylesheet FjssParser::parse_stylesheet() {
    while (!at_end()) {
        skip_whitespace();
        if (at_end()) break;

        // Skip comments
        if (peek() == '/' && pos_ + 1 < source_.size() && source_[pos_ + 1] == '*') {
            skip_comment();
            continue;
        }

        // Every iteration must consume input. A rule that fails to parse
        // recovers on its own, but this guard makes the loop's termination
        // independent of any single recovery path being complete.
        size_t rule_start = pos_;
        auto rules = parse_rule();
        for (auto& rule : rules) {
            if (!rule.properties.empty()) {
                sheet_.rules.push_back(std::move(rule));
            }
        }
        if (pos_ == rule_start) {
            diagnose(pos_, FjssSeverity::error,
                     std::string("unexpected '") + peek() + "'");
            advance();
        }
    }
    log_diagnostics();
    return std::move(sheet_);
}

std::vector<StyleRule> FjssParser::parse_rule() {
    skip_whitespace();
    size_t selector_start = pos_;
    std::vector<StyleSelector> selectors;
    while (true) {
        selectors.push_back(parse_selector());
        skip_whitespace();
        if (!at_end() && peek() == ',') {
            advance(); // ,
            continue;
        }
        break;
    }

    skip_whitespace();
    if (at_end() || peek() != '{') {
        // Point at the start of the rule the author needs to fix. When
        // nothing selector-like was read at all, the offending character
        // itself is the better clue.
        if (pos_ == selector_start && !at_end()) {
            diagnose(pos_, FjssSeverity::error, std::string("unexpected '") + peek() + "'");
        } else {
            diagnose(selector_start, FjssSeverity::error, "expected '{' after selector");
        }
        skip_malformed_rule();
        return {};
    }
    size_t block_start = pos_;
    advance(); // {

    std::unordered_map<std::string, std::string> properties;
    parse_declarations(properties);

    skip_whitespace();
    if (!at_end() && peek() == '}') {
        advance(); // }
    } else {
        diagnose(block_start, FjssSeverity::error, "rule is never closed, missing '}'");
    }

    std::vector<StyleRule> rules;
    rules.reserve(selectors.size());
    for (auto& selector : selectors) {
        rules.push_back({std::move(selector), properties});
    }
    return rules;
}

StyleSelector FjssParser::parse_selector() {
    StyleSelector sel;
    skip_whitespace();

    // Parse selector parts: tag, .class, #id, :pseudo, or ancestor descendant
    std::string current;
    bool parsing = true;
    while (!at_end() && parsing) {
        char c = peek();
        switch (c) {
        case '#': {
            if (!current.empty() && sel.tag.empty()) sel.tag = current;
            current.clear();
            advance(); // #
            while (!at_end() && (std::isalnum(static_cast<unsigned char>(peek())) ||
                   peek() == '-' || peek() == '_')) {
                current += advance();
            }
            sel.id = current;
            current.clear();
            break;
        }
        case '.': {
            if (!current.empty() && sel.tag.empty()) sel.tag = current;
            current.clear();
            advance(); // .
            while (!at_end() && (std::isalnum(static_cast<unsigned char>(peek())) ||
                   peek() == '-' || peek() == '_')) {
                current += advance();
            }
            sel.class_ = current;
            current.clear();
            break;
        }
        case ':': {
            if (!current.empty() && sel.tag.empty()) sel.tag = current;
            current.clear();
            advance(); // :
            while (!at_end() && (std::isalnum(static_cast<unsigned char>(peek())) ||
                   peek() == '-' || peek() == '_')) {
                current += advance();
            }
            sel.pseudo = current;
            current.clear();
            break;
        }
        case '{':
        case ',':
        case '}':
        case ';':
            // '}' and ';' can never start or continue a selector; leaving
            // them for the caller turns a stray one into a diagnostic
            // instead of an ancestor named "}".
            parsing = false;
            break;
        case ' ':
        case '\t':
        case '\n':
        case '\r': {
            if (!current.empty()) {
                // This could be a descendant combinator
                // Save current as potential parent, parse next part
                skip_whitespace();
                if (!at_end() && peek() != '{' && peek() != ',') {
                    sel.parent = current;
                    current.clear();
                } else {
                    parsing = false;
                }
            } else {
                skip_whitespace();
            }
            break;
        }
        default:
            current += advance();
            break;
        }
    }

    if (!current.empty()) {
        if (sel.tag.empty()) sel.tag = current;
    }

    return sel;
}

void FjssParser::parse_declarations(std::unordered_map<std::string, std::string>& props) {
    while (!at_end()) {
        skip_whitespace();
        if (at_end() || peek() == '}') break;

        // Skip comments inside block
        if (peek() == '/' && pos_ + 1 < source_.size() && source_[pos_ + 1] == '*') {
            skip_comment();
            continue;
        }

        // Property name
        size_t decl_start = pos_;
        std::string name;
        while (!at_end() && peek() != ':' && peek() != '}' && peek() != ';' &&
               !std::isspace(static_cast<unsigned char>(peek()))) {
            name += advance();
        }
        skip_whitespace();
        if (name.empty() || at_end() || peek() != ':') {
            // A bare ';' is harmless; anything else here is a declaration
            // missing its ':' and gets dropped.
            if (!name.empty()) {
                diagnose(decl_start, FjssSeverity::warning,
                         "expected ':' after '" + name + "'");
            } else if (!at_end() && peek() != ';') {
                diagnose(pos_, FjssSeverity::warning,
                         std::string("unexpected '") + peek() + "' in declaration block");
            }
            while (!at_end() && peek() != ';' && peek() != '}') advance();
            if (!at_end() && peek() == ';') advance();
            continue;
        }
        advance(); // :
        skip_whitespace();

        // Property value (everything until ; or })
        std::string value;
        while (!at_end() && peek() != ';' && peek() != '}') {
            value += advance();
        }
        // Trim trailing whitespace from value
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
            value.pop_back();
        }

        if (value.empty()) {
            diagnose(decl_start, FjssSeverity::warning, "'" + name + "' has no value");
        } else {
            props[name] = value;
        }

        if (!at_end() && peek() == ';') advance();
    }
}

void FjssParser::skip_whitespace() {
    while (!at_end() && std::isspace(static_cast<unsigned char>(peek()))) {
        advance();
    }
}

void FjssParser::skip_malformed_rule() {
    int depth = 0;
    while (!at_end()) {
        char c = advance();
        if (c == '{') {
            ++depth;
        } else if (c == '}') {
            if (depth <= 1) return;
            --depth;
        } else if (c == ';' && depth == 0) {
            return;
        }
    }
}

void FjssParser::skip_comment() {
    size_t comment_start = pos_;
    pos_ += 2; // skip /*
    while (pos_ + 1 < source_.size()) {
        if (source_[pos_] == '*' && source_[pos_ + 1] == '/') {
            pos_ += 2;
            return;
        }
        pos_++;
    }
    diagnose(comment_start, FjssSeverity::error, "comment is never closed, missing '*/'");
    pos_ = source_.size();
}

FjssParser::SourcePosition FjssParser::position_at(size_t offset) const {
    SourcePosition pos{1, 1};
    size_t end = std::min(offset, source_.size());
    for (size_t i = 0; i < end; ++i) {
        if (source_[i] == '\n') {
            ++pos.line;
            pos.column = 1;
        } else {
            ++pos.column;
        }
    }
    return pos;
}

void FjssParser::diagnose(size_t offset, FjssSeverity severity, std::string message) {
    auto pos = position_at(offset);
    sheet_.diagnostics.push_back({source_name_, pos.line, pos.column, severity, std::move(message)});
}

void FjssParser::log_diagnostics() const {
    for (const auto& d : sheet_.diagnostics) {
        const auto& name = d.source.empty() ? std::string_view{"<fjss>"} : std::string_view{d.source};
        if (d.severity == FjssSeverity::error) {
            FJELL_CORE_ERROR("{}:{}:{}: {}", name, d.line, d.column, d.message);
        } else {
            FJELL_CORE_WARN("{}:{}:{}: {}", name, d.line, d.column, d.message);
        }
    }
}

bool FjssParser::at_end() const {
    return pos_ >= source_.size();
}

char FjssParser::peek() const {
    return source_[pos_];
}

char FjssParser::advance() {
    return source_[pos_++];
}

} // namespace fjell
