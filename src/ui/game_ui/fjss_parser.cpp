#include "ui/game_ui/fjss_parser.hpp"
#include "ui/game_ui/ui_style_properties.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

namespace fjell {

uint32_t StyleSelector::specificity() const {
    uint32_t ids = 0;
    uint32_t classes = 0;
    uint32_t tags = 0;
    auto count = [&](const StyleCompound& c) {
        if (!c.id.empty()) ++ids;
        classes += static_cast<uint32_t>(c.classes.size() + c.pseudo_classes.size());
        if (!c.tag.empty()) ++tags;
    };
    count(subject);
    for (const auto& step : ancestors) count(step.compound);

    constexpr uint32_t FIELD_MAX = 255;
    return (std::min(ids, FIELD_MAX) << 16)
         | (std::min(classes, FIELD_MAX) << 8)
         |  std::min(tags, FIELD_MAX);
}

bool selects_only_id(const StyleSelector& selector, std::string_view id) {
    const auto& s = selector.subject;
    return s.id == id && s.tag.empty() && s.classes.empty() &&
           s.pseudo_classes.empty() && selector.ancestors.empty();
}

const StyleDeclaration* StyleRule::find(std::string_view name) const {
    for (auto it = declarations.rbegin(); it != declarations.rend(); ++it) {
        if (it->name == name) return &*it;
    }
    return nullptr;
}

StyleDeclaration* StyleRule::find(std::string_view name) {
    return const_cast<StyleDeclaration*>(std::as_const(*this).find(name));
}

void StyleRule::set(std::string_view name, std::string value) {
    if (auto* decl = find(name)) {
        decl->value = std::move(value);
        return;
    }
    declarations.push_back({std::string{name}, std::move(value), false});
}

void StyleRule::erase(std::string_view name) {
    std::erase_if(declarations, [&](const StyleDeclaration& d) { return d.name == name; });
}

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

        if (peek() == '@') {
            skip_at_rule();
            continue;
        }

        // Every iteration must consume input. A rule that fails to parse
        // recovers on its own, but this guard makes the loop's termination
        // independent of any single recovery path being complete.
        size_t rule_start = pos_;
        auto rules = parse_rule();
        for (auto& rule : rules) {
            if (!rule.declarations.empty()) {
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
    selector_problem_.clear();
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

    std::vector<StyleDeclaration> declarations;
    parse_declarations(declarations);

    skip_whitespace();
    if (!at_end() && peek() == '}') {
        advance(); // }
    } else {
        diagnose(block_start, FjssSeverity::error, "rule is never closed, missing '}'");
    }

    // As in CSS, one unsupported selector in a group invalidates the whole
    // rule rather than applying it to the selectors that did parse.
    if (!selector_problem_.empty()) {
        diagnose(selector_start, FjssSeverity::warning, selector_problem_ + ", rule ignored");
        return {};
    }

    std::vector<StyleRule> rules;
    rules.reserve(selectors.size());
    for (auto& selector : selectors) {
        rules.push_back({std::move(selector), declarations});
    }
    return rules;
}

StyleSelector FjssParser::parse_selector() {
    StyleSelector sel;
    StyleCompound current;
    // The combinator that will join `current` to the compound after it,
    // once that next compound starts. Whitespace alone means descendant.
    UiCombinator pending = UiCombinator::descendant;
    bool boundary = false; // whitespace or a combinator since `current` ended

    skip_whitespace();
    while (!at_end()) {
        char c = peek();
        if (c == '{' || c == ',' || c == '}' || c == ';') break;

        if (std::isspace(static_cast<unsigned char>(c))) {
            skip_whitespace();
            boundary = true;
            continue;
        }
        if (c == '>') {
            advance();
            if (current.empty()) {
                reject_selector("'>' has nothing on its left");
            }
            pending = UiCombinator::child;
            boundary = true;
            continue;
        }
        if (c == '+' || c == '~') {
            reject_selector(std::string("'") + advance() + "' combinator is not supported");
            boundary = true;
            continue;
        }

        if (boundary && !current.empty()) {
            sel.ancestors.push_back({std::move(current), pending});
            current = {};
            pending = UiCombinator::descendant;
        }
        boundary = false;
        parse_compound_part(current);
    }

    if (pending == UiCombinator::child) {
        reject_selector("'>' has nothing on its right");
    }
    sel.subject = std::move(current);
    return sel;
}

void FjssParser::parse_compound_part(StyleCompound& compound) {
    char c = peek();
    switch (c) {
    case '#':
        advance();
        compound.id = read_identifier();
        return;
    case '.':
        advance();
        compound.classes.push_back(read_identifier());
        return;
    case ':': {
        advance();
        bool pseudo_element = !at_end() && peek() == ':';
        if (pseudo_element) advance();
        auto name = read_identifier();
        if (pseudo_element) {
            reject_selector("'::" + name + "' pseudo-elements are not supported");
        } else if (!at_end() && peek() == '(') {
            // Functional pseudo-classes such as :not(.x) or :nth-child(2)
            int depth = 0;
            while (!at_end()) {
                char p = advance();
                if (p == '(') ++depth;
                else if (p == ')' && --depth == 0) break;
            }
            reject_selector("':" + name + "()' is not supported");
        } else {
            compound.pseudo_classes.push_back(name);
        }
        return;
    }
    case '*':
        advance();
        reject_selector("'*' universal selector is not supported");
        return;
    case '[':
        while (!at_end() && advance() != ']') {}
        reject_selector("attribute selectors are not supported");
        return;
    default:
        break;
    }

    if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') {
        compound.tag = read_identifier();
        return;
    }
    reject_selector(std::string("unexpected '") + advance() + "' in selector");
}

std::string FjssParser::read_identifier() {
    std::string ident;
    while (!at_end() && (std::isalnum(static_cast<unsigned char>(peek())) ||
                         peek() == '-' || peek() == '_')) {
        ident += advance();
    }
    return ident;
}

void FjssParser::reject_selector(std::string reason) {
    if (selector_problem_.empty()) selector_problem_ = std::move(reason);
}

void FjssParser::parse_declarations(std::vector<StyleDeclaration>& declarations) {
    while (!at_end()) {
        skip_whitespace();
        if (at_end() || peek() == '}') break;

        // Skip comments inside block
        if (peek() == '/' && pos_ + 1 < source_.size() && source_[pos_ + 1] == '*') {
            skip_comment();
            continue;
        }

        // A '{' before the next ';' or '}' means a rule nested inside this
        // block. Skipping its whole body keeps the outer rule's closing
        // brace from being mistaken for the nested one's.
        size_t next_delim = source_.find_first_of("{;}", pos_);
        if (next_delim != std::string_view::npos && source_[next_delim] == '{') {
            std::string head{source_.substr(pos_, next_delim - pos_)};
            while (!head.empty() && std::isspace(static_cast<unsigned char>(head.back()))) {
                head.pop_back();
            }
            diagnose(pos_, FjssSeverity::warning,
                     "nested rule '" + head + "' is not supported, ignored");
            pos_ = next_delim;
            skip_block();
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
        auto trim_end = [](std::string& s) {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
                s.pop_back();
            }
        };
        trim_end(value);

        bool important = false;
        constexpr std::string_view IMPORTANT = "!important";
        if (value.ends_with(IMPORTANT)) {
            important = true;
            value.resize(value.size() - IMPORTANT.size());
            trim_end(value);
        }

        if (value.empty()) {
            diagnose(decl_start, FjssSeverity::warning, "'" + name + "' has no value");
        } else {
            // The declaration is kept either way so the sheet round-trips
            // as written; the cascade ignores what it does not know, and
            // the report tells the author why nothing happened.
            validate_declaration(decl_start, name, value);
            declarations.push_back({name, value, important});
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

void FjssParser::validate_declaration(size_t offset, const std::string& name,
                                      const std::string& value) {
    const auto* desc = find_ui_style_property(name);
    if (!desc) {
        if (name.starts_with("--")) {
            diagnose(offset, FjssSeverity::warning,
                     "custom properties are not supported, '" + name + "' has no effect");
        } else {
            diagnose(offset, FjssSeverity::warning, "unknown property '" + name + "'");
        }
        return;
    }
    switch (desc->kind) {
    case UiPropertyKind::length:
    case UiPropertyKind::edges:
    case UiPropertyKind::edge:
        validate_lengths(offset, *desc, value);
        return;
    case UiPropertyKind::composite:
        if (name == "gap") validate_lengths(offset, *desc, value);
        return;
    case UiPropertyKind::keyword:
        break;
    case UiPropertyKind::number:
    case UiPropertyKind::color:
    case UiPropertyKind::corners:
        return;
    }

    for (auto keyword : desc->keywords) {
        if (keyword == value) return;
    }
    std::string expected;
    for (auto keyword : desc->keywords) {
        if (!expected.empty()) expected += ", ";
        expected += keyword;
    }
    diagnose(offset, FjssSeverity::warning,
             "'" + value + "' is not a value for '" + name + "', expected one of: " + expected);
}

void FjssParser::validate_lengths(size_t offset, const UiPropertyDescriptor& desc,
                                  const std::string& value) {
    // A function such as calc() is one value, not a list of lengths.
    if (value.find('(') != std::string::npos) {
        diagnose(offset, FjssSeverity::warning,
                 "'" + value + "' is not a length for '" + std::string(desc.name) + "'");
        return;
    }
    std::istringstream tokens(value);
    std::string token;
    while (tokens >> token) {
        if (token == "auto") {
            if (!desc.auto_allowed) {
                diagnose(offset, FjssSeverity::warning,
                         "'auto' is not a value for '" + std::string(desc.name) + "'");
            }
            continue;
        }
        // A number, optionally followed by px or %
        size_t i = 0;
        if (i < token.size() && (token[i] == '-' || token[i] == '+')) ++i;
        size_t digits_start = i;
        while (i < token.size() && (std::isdigit(static_cast<unsigned char>(token[i])) || token[i] == '.')) ++i;
        std::string_view unit{token.data() + i, token.size() - i};
        if (i == digits_start) {
            diagnose(offset, FjssSeverity::warning,
                     "'" + token + "' is not a length for '" + std::string(desc.name) + "'");
        } else if (!unit.empty() && unit != "px" && unit != "%") {
            diagnose(offset, FjssSeverity::warning,
                     "unit '" + std::string(unit) + "' is not supported for '" +
                     std::string(desc.name) + "', use px or %");
        }
    }
}

void FjssParser::skip_block() {
    int depth = 0;
    while (!at_end()) {
        char c = advance();
        if (c == '{') {
            ++depth;
        } else if (c == '}' && --depth == 0) {
            return;
        }
    }
}

void FjssParser::skip_at_rule() {
    size_t start = pos_;
    advance(); // @
    std::string name;
    while (!at_end() && (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '-')) {
        name += advance();
    }
    diagnose(start, FjssSeverity::warning, "'@" + name + "' is not supported, ignored");

    // A statement at-rule ends at ';', a block at-rule at its closing brace.
    while (!at_end()) {
        char c = peek();
        if (c == ';') {
            advance();
            return;
        }
        if (c == '{') {
            skip_block();
            return;
        }
        advance();
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
