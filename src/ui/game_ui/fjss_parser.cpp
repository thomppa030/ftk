#include "ui/game_ui/fjss_parser.hpp"
#include "ui/game_ui/ui_style_properties.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <charconv>
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

static void append_compound(const StyleCompound& c, std::string& out) {
    out += c.tag;
    if (!c.id.empty()) out += "#" + c.id;
    for (const auto& cls : c.classes) out += "." + cls;
    for (const auto& pseudo : c.pseudo_classes) out += ":" + pseudo;
}

std::string to_css(const StyleSelector& selector) {
    std::string out;
    for (const auto& step : selector.ancestors) {
        append_compound(step.compound, out);
        out += step.combinator == UiCombinator::child ? " > " : " ";
    }
    append_compound(selector.subject, out);
    return out;
}

bool MediaCondition::holds(float viewport_width, float viewport_height) const {
    float aspect = viewport_height > 0.0f ? viewport_width / viewport_height : 0.0f;
    switch (feature) {
    case MediaFeature::min_width: return viewport_width >= value;
    case MediaFeature::max_width: return viewport_width <= value;
    case MediaFeature::min_height: return viewport_height >= value;
    case MediaFeature::max_height: return viewport_height <= value;
    case MediaFeature::min_aspect_ratio: return aspect >= value;
    case MediaFeature::max_aspect_ratio: return aspect <= value;
    case MediaFeature::orientation:
        return portrait ? viewport_height > viewport_width : viewport_width >= viewport_height;
    }
    return true;
}

bool media_holds(const std::vector<MediaCondition>& conditions,
                 float viewport_width, float viewport_height) {
    for (const auto& condition : conditions) {
        if (!condition.holds(viewport_width, viewport_height)) return false;
    }
    return true;
}

static const char* media_feature_name(MediaFeature feature) {
    switch (feature) {
    case MediaFeature::min_width: return "min-width";
    case MediaFeature::max_width: return "max-width";
    case MediaFeature::min_height: return "min-height";
    case MediaFeature::max_height: return "max-height";
    case MediaFeature::min_aspect_ratio: return "min-aspect-ratio";
    case MediaFeature::max_aspect_ratio: return "max-aspect-ratio";
    case MediaFeature::orientation: return "orientation";
    }
    return "";
}

std::string to_css(const std::vector<MediaCondition>& conditions) {
    std::string out;
    for (const auto& c : conditions) {
        if (!out.empty()) out += " and ";
        out += "(";
        out += media_feature_name(c.feature);
        out += ": ";
        if (c.feature == MediaFeature::orientation) {
            out += c.portrait ? "portrait" : "landscape";
        } else {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(c.value));
            out += buf;
        }
        out += ")";
    }
    return out;
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
    parse_rules(false);
    log_diagnostics();
    return std::move(sheet_);
}

void FjssParser::parse_rules(bool in_block) {
    while (!at_end()) {
        skip_whitespace();
        if (at_end()) break;
        if (in_block && peek() == '}') return;

        // Skip comments
        if (peek() == '/' && pos_ + 1 < source_.size() && source_[pos_ + 1] == '*') {
            skip_comment();
            continue;
        }

        if (peek() == '@') {
            parse_at_rule();
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
        rules.push_back({std::move(selector), declarations, source_name_, media_stack_});
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
        } else if (name == "hover" || name == "active" || name == "focus" || name == "root") {
            compound.pseudo_classes.push_back(name);
        } else {
            reject_selector("':" + name + "' pseudo-class is not supported");
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
    // A custom property takes any value; a value built from one can only
    // be checked once it is substituted, at resolve time.
    if (name.starts_with("--") || value.find("var(") != std::string::npos) return;

    const auto* desc = find_ui_style_property(name);
    if (!desc) {
        diagnose(offset, FjssSeverity::warning, "unknown property '" + name + "'");
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
        if (name == "text-shadow") validate_text_shadow(offset, value);
        return;
    case UiPropertyKind::keyword:
        break;
    case UiPropertyKind::number:
        if (name == "font-size") validate_lengths(offset, *desc, value);
        if (name == "font-weight" && !parse_ui_font_weight(value)) {
            diagnose(offset, FjssSeverity::warning,
                     "'" + value + "' is not a font weight, expected 1-1000, normal or bold");
        }
        return;
    case UiPropertyKind::color:
    case UiPropertyKind::corners:
    case UiPropertyKind::string:
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

void FjssParser::validate_text_shadow(size_t offset, const std::string& value) {
    if (value == "none") return;
    size_t lengths = 0;
    size_t colors = 0;
    for (const auto& token : ui_style_value_tokens(value)) {
        if (ui_style_is_length_token(token)) {
            ++lengths;
        } else {
            ++colors;
        }
    }
    if (lengths < 2 || lengths > 3 || colors != 1) {
        diagnose(offset, FjssSeverity::warning,
                 "'" + value + "' is not a text shadow, expected 'x y [blur] color' or none");
    }
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
        bool known_unit = unit.empty() || unit == "px";
        for (UiUnit u : {UiUnit::percent, UiUnit::em, UiUnit::rem, UiUnit::vw, UiUnit::vh,
                         UiUnit::vmin, UiUnit::vmax}) {
            if (unit == unit_suffix(u)) known_unit = true;
        }
        if (i == digits_start) {
            diagnose(offset, FjssSeverity::warning,
                     "'" + token + "' is not a length for '" + std::string(desc.name) + "'");
        } else if (!known_unit) {
            diagnose(offset, FjssSeverity::warning,
                     "unit '" + std::string(unit) + "' is not supported for '" +
                     std::string(desc.name) + "', use px, %, em, rem, vw or vh");
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

void FjssParser::parse_import(size_t start) {
    skip_whitespace();
    // `@import "file";` or `@import url("file");`
    bool in_url = false;
    if (source_.substr(pos_).starts_with("url(")) {
        pos_ += 4;
        in_url = true;
        skip_whitespace();
    }
    if (at_end() || (peek() != '"' && peek() != '\'')) {
        diagnose(start, FjssSeverity::error, "expected a quoted file name after @import");
        skip_malformed_rule();
        return;
    }
    char quote = advance();
    std::string path;
    while (!at_end() && peek() != quote && peek() != '\n') path += advance();
    if (at_end() || peek() != quote) {
        diagnose(start, FjssSeverity::error, "@import file name is not closed");
        skip_malformed_rule();
        return;
    }
    advance(); // closing quote
    skip_whitespace();
    if (in_url && !at_end() && peek() == ')') {
        advance();
        skip_whitespace();
    }
    if (!at_end() && peek() == ';') advance();

    auto position = position_at(start);
    sheet_.imports.push_back({path, source_name_, sheet_.rules.size(), position.line, position.column});
}

bool FjssParser::parse_media_query(size_t start, std::vector<MediaCondition>& conditions) {
    bool ok = true;
    while (!at_end()) {
        skip_whitespace();
        if (at_end() || peek() == '{') break;

        if (peek() == '(') {
            advance();
            std::string inner;
            while (!at_end() && peek() != ')' && peek() != '{') inner += advance();
            if (at_end() || peek() != ')') {
                diagnose(start, FjssSeverity::error, "@media condition is not closed");
                return false;
            }
            advance(); // )
            auto colon = inner.find(':');
            auto trim = [](std::string text) {
                auto b = text.find_first_not_of(" \t");
                auto e = text.find_last_not_of(" \t");
                return b == std::string::npos ? std::string{} : text.substr(b, e - b + 1);
            };
            std::string feature = trim(inner.substr(0, colon));
            std::string value = colon == std::string::npos ? "" : trim(inner.substr(colon + 1));

            MediaCondition condition;
            if (feature == "min-width") condition.feature = MediaFeature::min_width;
            else if (feature == "max-width") condition.feature = MediaFeature::max_width;
            else if (feature == "min-height") condition.feature = MediaFeature::min_height;
            else if (feature == "max-height") condition.feature = MediaFeature::max_height;
            else if (feature == "min-aspect-ratio") condition.feature = MediaFeature::min_aspect_ratio;
            else if (feature == "max-aspect-ratio") condition.feature = MediaFeature::max_aspect_ratio;
            else if (feature == "orientation") condition.feature = MediaFeature::orientation;
            else {
                diagnose(start, FjssSeverity::warning,
                         "unknown media feature '" + feature + "', block ignored");
                ok = false;
                continue;
            }

            if (condition.feature == MediaFeature::orientation) {
                if (value == "portrait") condition.portrait = true;
                else if (value == "landscape") condition.portrait = false;
                else {
                    diagnose(start, FjssSeverity::warning,
                             "orientation must be portrait or landscape, block ignored");
                    ok = false;
                }
            } else if (condition.feature == MediaFeature::min_aspect_ratio ||
                       condition.feature == MediaFeature::max_aspect_ratio) {
                float w = 0.0f;
                float h = 1.0f;
                auto slash = value.find('/');
                auto [end, ec] = std::from_chars(value.data(), value.data() + std::min(slash, value.size()), w);
                if (ec != std::errc{}) {
                    diagnose(start, FjssSeverity::warning,
                             "'" + value + "' is not an aspect ratio, block ignored");
                    ok = false;
                } else if (slash != std::string::npos) {
                    std::from_chars(value.data() + slash + 1, value.data() + value.size(), h);
                }
                condition.value = h > 0.0f ? w / h : 0.0f;
            } else {
                float px = 0.0f;
                auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), px);
                std::string_view unit{end, static_cast<size_t>(value.data() + value.size() - end)};
                if (ec != std::errc{} || (!unit.empty() && unit != "px")) {
                    diagnose(start, FjssSeverity::warning,
                             "'" + value + "' is not a length in pixels for '" + feature + "', block ignored");
                    ok = false;
                }
                condition.value = px;
            }
            conditions.push_back(condition);
            continue;
        }

        auto word = read_identifier();
        if (word == "and" || word == "screen" || word == "all") continue;
        if (word == "not" || word == "only") {
            diagnose(start, FjssSeverity::warning, "'" + word + "' in @media is not supported, block ignored");
            ok = false;
            continue;
        }
        if (word.empty()) {
            char c = advance();
            if (c == ',') {
                diagnose(start, FjssSeverity::warning,
                         "comma-separated @media queries are not supported, block ignored");
            } else {
                diagnose(start, FjssSeverity::warning,
                         std::string("unexpected '") + c + "' in @media query, block ignored");
            }
            ok = false;
        }
    }
    return ok;
}

void FjssParser::parse_media(size_t start) {
    std::vector<MediaCondition> conditions;
    bool ok = parse_media_query(start, conditions);
    skip_whitespace();
    if (at_end() || peek() != '{') {
        diagnose(start, FjssSeverity::error, "expected '{' after @media query");
        skip_malformed_rule();
        return;
    }
    if (!ok) {
        skip_block();
        return;
    }

    advance(); // {
    size_t outer = media_stack_.size();
    media_stack_.insert(media_stack_.end(), conditions.begin(), conditions.end());
    parse_rules(true);
    media_stack_.resize(outer);
    if (!at_end() && peek() == '}') {
        advance();
    } else {
        diagnose(start, FjssSeverity::error, "@media block is never closed, missing '}'");
    }
}

void FjssParser::parse_font_face(size_t start) {
    skip_whitespace();
    if (at_end() || peek() != '{') {
        diagnose(start, FjssSeverity::error, "expected '{' after @font-face");
        skip_malformed_rule();
        return;
    }
    advance(); // {

    // The block holds descriptors, not style properties, so it is read
    // here rather than through parse_declarations and its property table.
    StyleFontFace face;
    auto position = position_at(start);
    face.source = source_name_;
    face.line = position.line;
    face.column = position.column;
    bool has_family = false;
    bool has_src = false;

    auto strip_quotes = [](std::string text) {
        if (text.size() >= 2 && (text.front() == '"' || text.front() == '\'') &&
            text.back() == text.front()) {
            return text.substr(1, text.size() - 2);
        }
        return text;
    };

    while (!at_end()) {
        skip_whitespace();
        if (at_end() || peek() == '}') break;
        if (peek() == '/' && pos_ + 1 < source_.size() && source_[pos_ + 1] == '*') {
            skip_comment();
            continue;
        }
        size_t decl_start = pos_;
        std::string name;
        while (!at_end() && peek() != ':' && peek() != '}' && peek() != ';' &&
               !std::isspace(static_cast<unsigned char>(peek()))) {
            name += advance();
        }
        skip_whitespace();
        if (name.empty() || at_end() || peek() != ':') {
            if (!name.empty()) {
                diagnose(decl_start, FjssSeverity::warning,
                         "expected ':' after '" + name + "' in @font-face");
            }
            while (!at_end() && peek() != ';' && peek() != '}') advance();
            if (!at_end() && peek() == ';') advance();
            continue;
        }
        advance(); // :
        skip_whitespace();
        std::string value;
        while (!at_end() && peek() != ';' && peek() != '}') value += advance();
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
            value.pop_back();
        }
        if (!at_end() && peek() == ';') advance();

        if (name == "font-family") {
            face.family = strip_quotes(value);
            has_family = !face.family.empty();
        } else if (name == "src") {
            if (value.starts_with("url(") && value.ends_with(")")) {
                value = value.substr(4, value.size() - 5);
                while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
                    value.erase(value.begin());
                }
                while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
                    value.pop_back();
                }
            }
            face.src = strip_quotes(value);
            has_src = !face.src.empty();
        } else if (name == "font-weight") {
            auto weight = parse_ui_font_weight(value);
            if (!weight) {
                diagnose(decl_start, FjssSeverity::warning,
                         "'" + value + "' is not a font weight, expected 1-1000, normal or bold");
            } else {
                face.weight = *weight;
            }
        } else {
            diagnose(decl_start, FjssSeverity::warning,
                     "'" + name + "' is not a @font-face descriptor, ignored");
        }
    }

    if (!at_end() && peek() == '}') {
        advance();
    } else {
        diagnose(start, FjssSeverity::error, "@font-face block is never closed, missing '}'");
    }

    if (!has_family || !has_src) {
        diagnose(start, FjssSeverity::error,
                 std::string("@font-face needs ") + (!has_family ? "a font-family" : "a src") +
                 ", block ignored");
        return;
    }
    sheet_.font_faces.push_back(std::move(face));
}

void FjssParser::parse_at_rule() {
    size_t start = pos_;
    advance(); // @
    std::string name;
    while (!at_end() && (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '-')) {
        name += advance();
    }
    if (name == "import") {
        parse_import(start);
        return;
    }
    if (name == "media") {
        parse_media(start);
        return;
    }
    if (name == "font-face") {
        parse_font_face(start);
        return;
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
    // "file:line:column: severity: message" — the compiler convention, which
    // editors and their linter plugins parse without configuration.
    for (const auto& d : sheet_.diagnostics) {
        const auto& name = d.source.empty() ? std::string_view{"<fjss>"} : std::string_view{d.source};
        if (d.severity == FjssSeverity::error) {
            FJELL_CORE_ERROR("{}:{}:{}: error: {}", name, d.line, d.column, d.message);
        } else {
            FJELL_CORE_WARN("{}:{}:{}: warning: {}", name, d.line, d.column, d.message);
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
