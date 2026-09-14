#include "ui/game_ui/fjss_parser.hpp"
#include "core/log.hpp"

#include <cctype>

namespace fjell {

FjssParser::FjssParser(std::string_view source) : source_{source} {}

Stylesheet FjssParser::parse(std::string_view source) {
    FjssParser parser{source};
    return parser.parse_stylesheet();
}

Stylesheet FjssParser::parse_stylesheet() {
    Stylesheet sheet;
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
                sheet.rules.push_back(std::move(rule));
            }
        }
        if (pos_ == rule_start) {
            FJELL_CORE_WARN("fjss: unexpected '{}', skipping", peek());
            advance();
        }
    }
    return sheet;
}

std::vector<StyleRule> FjssParser::parse_rule() {
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
        FJELL_CORE_WARN("fjss: expected '{{' after selector");
        skip_malformed_rule();
        return {};
    }
    advance(); // {

    std::unordered_map<std::string, std::string> properties;
    parse_declarations(properties);

    skip_whitespace();
    if (!at_end() && peek() == '}') {
        advance(); // }
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
        std::string name;
        while (!at_end() && peek() != ':' && peek() != '}' && peek() != ';' &&
               !std::isspace(static_cast<unsigned char>(peek()))) {
            name += advance();
        }
        skip_whitespace();
        if (name.empty() || at_end() || peek() != ':') {
            // Skip malformed declaration
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

        if (!name.empty() && !value.empty()) {
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
    pos_ += 2; // skip /*
    while (pos_ + 1 < source_.size()) {
        if (source_[pos_] == '*' && source_[pos_ + 1] == '/') {
            pos_ += 2;
            return;
        }
        pos_++;
    }
    pos_ = source_.size();
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
