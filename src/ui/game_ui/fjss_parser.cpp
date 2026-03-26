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

        auto rule = parse_rule();
        if (!rule.properties.empty()) {
            sheet.rules.push_back(std::move(rule));
        }
    }
    return sheet;
}

StyleRule FjssParser::parse_rule() {
    StyleRule rule;
    rule.selector = parse_selector();

    skip_whitespace();
    if (at_end() || peek() != '{') {
        FJELL_CORE_WARN("fjss: expected '{{' after selector");
        return rule;
    }
    advance(); // {

    parse_declarations(rule.properties);

    skip_whitespace();
    if (!at_end() && peek() == '}') {
        advance(); // }
    }

    return rule;
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
