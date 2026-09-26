#include "parser.hpp"

#include "lexer.hpp"
#include "types.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace fjreflect {

/// Why a field's [[fj::shader_type]] cannot stand, or empty when it can. The
/// Type names themselves are the engine's to check when it loads the module:
/// it owns the list, and a copy here would drift from it.
static std::string shader_type_error(const FieldInfo& fi) {
    if (fi.shader_types.empty()) return {};
    if (fi.fj_field_type == "FJELL_FIELD_REF" && fi.ref_target == "Material") return {};
    return "[[fj::shader_type]] requires Ref<Material>, got '" + fi.cpp_type + "' on '" +
           fi.name + "'";
}

// ── Header parsing ──────────────────────────────────────────────────────

namespace {

using TokenList = std::vector<Token>;

bool is_word(const Token& t) {
    return t.kind == TokenKind::identifier || t.kind == TokenKind::number ||
           t.kind == TokenKind::string || t.kind == TokenKind::character;
}

/// Tokens written back out the way the engine's own code writes a type: a
/// space between two words and after a comma, none around `::`, `<`, `>`, `*`
/// or `&`. `fjell::WeakRef<fjell::Node3D>`, `const int&`, `std::map<int, float>`.
std::string join(const TokenList& tokens, std::size_t begin, std::size_t end) {
    std::string out;
    for (std::size_t i = begin; i < end; ++i) {
        const Token& t = tokens[i];
        if (i > begin) {
            const Token& prev = tokens[i - 1];
            const bool space = (is_word(prev) && is_word(t)) || prev.text == "," ||
                               ((prev.text == ">" || prev.text == "*" || prev.text == "&") &&
                                t.kind == TokenKind::identifier);
            if (space) out += ' ';
        }
        out += t.text;
    }
    return out;
}

/// The text between a string literal's quotes, prefix and suffix dropped.
std::string string_body(const Token& t) {
    const auto open = t.text.find('"');
    const auto close = t.text.rfind('"');
    if (open == std::string::npos || close <= open) return {};
    if (open > 0 && t.text[open - 1] == 'R') {
        const auto paren = t.text.find('(', open);
        const auto end = t.text.rfind(')', close);
        return t.text.substr(paren + 1, end - paren - 1);
    }
    return t.text.substr(open + 1, close - open - 1);
}

/// Looks like a macro rather than a type or a function: all capitals, digits
/// and underscores.
bool is_macro_name(const Token& t) {
    if (t.kind != TokenKind::identifier || t.text.size() < 2) return false;
    bool letter = false;
    for (char c : t.text) {
        if (std::isupper(static_cast<unsigned char>(c))) letter = true;
        else if (!std::isdigit(static_cast<unsigned char>(c)) && c != '_') return false;
    }
    return letter;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

struct Attribute {
    std::string scope; // "fj" for [[fj::range(...)]], empty for [[nodiscard]]
    std::string name;
    TokenList args;    // the tokens between the parentheses
    bool has_args{false};
    unsigned line{0};
};

enum class ComponentMacro { none, component, ui_widget, notify };

/// One declaration inside a class body, as far as reflection cares.
struct Member {
    std::vector<Attribute> attrs;
    bool is_function{false};
    bool is_static{false};
    bool is_array{false};
    bool several_declarators{false};
    bool unsupported_declarator{false}; // a function pointer or parenthesised name
    bool has_body{false};
    bool is_const{false};               // a const member function
    bool is_public{true};
    std::string name;
    unsigned line{0};
    std::string type;                   // data type, or the function's return type
    std::vector<std::string> param_types; // const and references stripped
    std::vector<std::string> param_names; // empty where the parameter has none
};

/// A class being read. Members are kept until the class closes, because the
/// macro that makes it a component may come anywhere in its body.
struct ClassScope {
    std::string name;
    std::string qualified_name;
    unsigned line{0};
    bool is_struct{false};    // members are public until an access specifier says otherwise
    ComponentMacro macro{ComponentMacro::none};
    std::string macro_name;   // the class name the macro was given
    std::string widget_tag;
    unsigned macro_line{0};
    std::vector<Member> members;
    std::vector<Attribute> attrs;  // on the class head: class [[fj::tick(...)]] Name
};

class HeaderParser {
public:
    HeaderParser(std::string path, TokenList tokens, ParseResult& result)
        : path_(std::move(path)), tokens_(std::move(tokens)), result_(result) {}

    void run() { parse_scope("", false); }

private:
    // ── Token access ────────────────────────────────────────────────────

    [[nodiscard]] const Token& tok(std::size_t ahead = 0) const {
        const std::size_t i = std::min(pos_ + ahead, tokens_.size() - 1);
        return tokens_[i];
    }
    [[nodiscard]] bool at_end() const { return tok().kind == TokenKind::end; }
    /// The next token is this punctuation mark or name (never a literal).
    [[nodiscard]] bool is(std::string_view text, std::size_t ahead = 0) const {
        const Token& t = tok(ahead);
        return (t.kind == TokenKind::punct || t.kind == TokenKind::identifier) && t.text == text;
    }
    [[nodiscard]] bool at_attribute() const { return is("[") && is("[", 1); }
    void next() {
        if (!at_end()) ++pos_;
    }

    void error(unsigned line, const std::string& message) {
        result_.errors.push_back(path_ + ":" + std::to_string(line) + ": error: " + message);
    }
    void warning(unsigned line, const std::string& message) {
        result_.warnings.push_back(path_ + ":" + std::to_string(line) + ": warning: " + message);
    }

    // ── Skipping what reflection does not read ──────────────────────────

    /// At an opening bracket: step past its matching close, whatever nests
    /// inside. Returns false at the end of the file.
    bool skip_balanced() {
        std::string closers;
        const unsigned line = tok().line;
        do {
            const std::string& t = tok().text;
            if (tok().kind == TokenKind::punct) {
                if (t == "(") closers.push_back(')');
                else if (t == "[") closers.push_back(']');
                else if (t == "{") closers.push_back('}');
                else if ((t == ")" || t == "]" || t == "}") && !closers.empty() &&
                         t[0] == closers.back()) {
                    closers.pop_back();
                }
            }
            if (at_end()) {
                error(line, "unexpected end of file: this bracket is never closed");
                return false;
            }
            next();
        } while (!closers.empty());
        return true;
    }

    /// At `<`: step past the matching `>`. Brackets inside are skipped whole,
    /// so a `>` in a default argument's parentheses does not close the list.
    void skip_angles() {
        int depth = 0;
        do {
            if (is("(") || is("[") || is("{")) {
                if (!skip_balanced()) return;
                continue;
            }
            if (is("<")) ++depth;
            else if (is(">")) --depth;
            else if (is(";") || is("}") || at_end()) return;
            next();
        } while (depth > 0);
    }

    /// Up to and past the `;` that ends this declaration. Stops short of a
    /// `}` that closes the enclosing scope.
    void skip_to_semicolon() {
        while (!at_end() && !is(";") && !is("}")) {
            if (is("(") || is("[") || is("{")) {
                if (!skip_balanced()) return;
            } else {
                next();
            }
        }
        if (is(";")) next();
    }

    /// Past a declaration nobody reflects: to its `;`, or past its body.
    void skip_declaration() {
        const Token* prev = nullptr;
        while (!at_end() && !is(";") && !is("}")) {
            if (is("{")) {
                const bool initializer = prev != nullptr && (prev->text == "=" || prev->text == ",");
                if (!skip_balanced()) return;
                if (!initializer) {
                    if (is(";")) next();
                    return;
                }
                prev = nullptr;
                continue;
            }
            if (is("(") || is("[")) {
                if (!skip_balanced()) return;
                prev = nullptr;
                continue;
            }
            prev = &tok();
            next();
        }
        if (is(";")) next();
    }

    /// At `template`: past the parameter list and the declaration it heads.
    void skip_template() {
        next();
        if (is("<")) skip_angles();
        if ((is("class") || is("struct") || is("union")) && class_head_is_definition()) {
            while (!at_end() && !is("{")) next();
            skip_balanced();
            skip_to_semicolon();
            return;
        }
        skip_declaration();
    }

    /// At an ALL_CAPS name followed by `(`: past the invocation.
    void skip_macro_invocation() {
        next();
        skip_balanced();
        if (is(";")) next();
    }

    // ── Attributes ──────────────────────────────────────────────────────

    /// Every `[[...]]` block and `alignas(...)` here, in order.
    std::vector<Attribute> parse_attributes() {
        std::vector<Attribute> attrs;
        while (true) {
            if (is("alignas") && is("(", 1)) {
                next();
                skip_balanced();
                continue;
            }
            if (!at_attribute()) break;
            next();
            next();
            std::string using_scope;
            if (is("using") && tok(1).kind == TokenKind::identifier && is(":", 2)) {
                using_scope = tok(1).text;
                next();
                next();
                next();
            }
            while (!at_end() && !(is("]") && is("]", 1))) {
                if (is(",")) {
                    next();
                    continue;
                }
                if (tok().kind != TokenKind::identifier) {
                    error(tok().line, "expected an attribute name, found '" + tok().text + "'");
                    while (!at_end() && !(is("]") && is("]", 1))) next();
                    break;
                }
                Attribute a;
                a.line = tok().line;
                a.scope = using_scope;
                a.name = tok().text;
                next();
                if (is("::") && tok(1).kind == TokenKind::identifier) {
                    a.scope = a.name;
                    a.name = tok(1).text;
                    next();
                    next();
                }
                if (is("(")) {
                    a.has_args = true;
                    const std::size_t open = pos_;
                    if (!skip_balanced()) break;
                    a.args.assign(tokens_.begin() + static_cast<std::ptrdiff_t>(open) + 1,
                                  tokens_.begin() + static_cast<std::ptrdiff_t>(pos_) - 1);
                }
                attrs.push_back(std::move(a));
            }
            next();
            next();
        }
        return attrs;
    }

    // ── Scopes ──────────────────────────────────────────────────────────

    /// `class Name ... {` or `class Name : Base {` defines a class here;
    /// `class Name;` declares one, and `class Name* member` only names one.
    [[nodiscard]] bool class_head_is_definition() const {
        std::size_t i = 1;
        auto at = [&](std::string_view text) {
            const Token& t = tok(i);
            return (t.kind == TokenKind::punct || t.kind == TokenKind::identifier) && t.text == text;
        };
        while (at("[") && tok(i + 1).text == "[") {
            int depth = 0;
            do {
                if (at("[")) ++depth;
                else if (at("]")) --depth;
                ++i;
            } while (depth > 0 && tok(i).kind != TokenKind::end);
        }
        while (tok(i).kind == TokenKind::identifier || at("::")) {
            if (at("final")) {
                ++i;
                break;
            }
            if (at("alignas") && tok(i + 1).text == "(") {
                int depth = 0;
                ++i;
                do {
                    if (at("(")) ++depth;
                    else if (at(")")) --depth;
                    ++i;
                } while (depth > 0 && tok(i).kind != TokenKind::end);
                continue;
            }
            ++i;
            if (at("<")) {
                int depth = 0;
                do {
                    if (at("<")) ++depth;
                    else if (at(">")) --depth;
                    ++i;
                } while (depth > 0 && tok(i).kind != TokenKind::end);
            }
        }
        return at("{") || at(":");
    }

    /// Declarations at namespace level, until the closing `}` when braced.
    void parse_scope(const std::string& ns, bool braced) {
        const unsigned open_line = pos_ > 0 ? tokens_[pos_ - 1].line : 1;
        while (true) {
            if (at_end()) {
                if (braced) error(open_line, "unexpected end of file: this '{' is never closed");
                return;
            }
            if (is("}")) {
                if (braced) {
                    next();
                    return;
                }
                error(tok().line, "unmatched '}'");
                next();
                continue;
            }
            if (is(";")) {
                next();
            } else if (is("namespace") || (is("inline") && is("namespace", 1))) {
                parse_namespace(ns);
            } else if (is("extern") && tok(1).kind == TokenKind::string) {
                next();
                next();
                if (is("{")) {
                    next();
                    parse_scope(ns, true);
                }
            } else if (is("template")) {
                skip_template();
            } else if ((is("class") || is("struct") || is("union")) && class_head_is_definition()) {
                parse_class(ns);
            } else if (is("using") || is("typedef") || is("static_assert")) {
                skip_to_semicolon();
            } else if (at_attribute()) {
                auto attrs = parse_attributes();
                const bool pipe = std::any_of(attrs.begin(), attrs.end(), [](const Attribute& a) {
                    return a.scope == "fj" && a.name == "pipe";
                });
                if (pipe) {
                    parse_pipe(ns, std::move(attrs));
                    continue;
                }
                for (const auto& a : attrs) {
                    if (a.scope == "fj") {
                        error(a.line, "[[fj::" + a.name + "]] only means something inside a component");
                    }
                }
            } else if (is_macro_name(tok()) && is("(", 1)) {
                skip_macro_invocation();
            } else {
                skip_declaration();
            }
        }
    }

    void parse_namespace(const std::string& ns) {
        if (is("inline")) next();
        next(); // namespace
        parse_attributes();
        std::string name;
        while (tok().kind == TokenKind::identifier || is("::")) {
            if (is("inline")) {
                next();
                continue;
            }
            name += tok().text;
            next();
        }
        if (is("=")) { // namespace alias
            skip_to_semicolon();
            return;
        }
        if (!is("{")) {
            error(tok().line, "expected '{' after namespace " + name);
            skip_declaration();
            return;
        }
        next();
        const std::string inner = name.empty() ? ns : (ns.empty() ? name : ns + "::" + name);
        parse_scope(inner, true);
    }

    /// At `class`, `struct` or `union` that defines a class.
    void parse_class(const std::string& outer) {
        ClassScope cls;
        cls.line = tok().line;
        cls.is_struct = !is("class");
        next();
        cls.attrs = parse_attributes();
        while (tok().kind == TokenKind::identifier || is("::")) {
            if (is("final")) {
                next();
                break;
            }
            if (is("alignas")) {
                parse_attributes();
                continue;
            }
            cls.name = is("::") ? cls.name : tok().text;
            next();
            if (is("<")) skip_angles(); // a specialisation: its arguments
        }
        if (is(":")) {
            while (!at_end() && !is("{") && !is(";")) {
                if (is("(")) skip_balanced();
                else next();
            }
        }
        if (!is("{")) {
            error(tok().line, "expected '{' to open class " + cls.name);
            skip_declaration();
            return;
        }
        next();
        cls.qualified_name = outer.empty() ? cls.name : outer + "::" + cls.name;
        parse_class_body(cls);
        skip_to_semicolon(); // `};`, or `} instance;`
        finish_class(cls);
    }

    /// Members until the class's closing brace, which it consumes.
    void parse_class_body(ClassScope& cls) {
        bool public_access = cls.is_struct;
        while (true) {
            if (at_end()) {
                error(cls.line, "unexpected end of file: class " + cls.name + " is never closed");
                return;
            }
            if (is("}")) {
                next();
                return;
            }
            if (is(";")) {
                next();
            } else if ((is("public") || is("private") || is("protected")) && is(":", 1)) {
                public_access = is("public");
                next();
                next();
            } else if (is("FJELL_COMPONENT") || is("FJELL_UI_WIDGET") || is("FJELL_UI_COMPONENT") ||
                       is("FJELL_NOTIFY")) {
                parse_component_macro(cls);
                public_access = true; // each macro ends in `public:`
            } else if (is_macro_name(tok()) && is("(", 1)) {
                skip_macro_invocation();
            } else if ((is("class") || is("struct") || is("union")) && class_head_is_definition()) {
                parse_class(cls.qualified_name);
            } else if (is("enum") || is("friend")) {
                skip_declaration();
            } else if (is("using") || is("typedef") || is("static_assert")) {
                skip_to_semicolon();
            } else if (is("template")) {
                skip_template();
            } else {
                Member m;
                m.attrs = parse_attributes();
                m.is_public = public_access;
                if (read_declaration(m)) cls.members.push_back(std::move(m));
            }
        }
    }

    void parse_component_macro(ClassScope& cls) {
        const Token& macro = tok();
        const unsigned line = macro.line;
        ComponentMacro kind = ComponentMacro::component;
        if (macro.text == "FJELL_UI_WIDGET" || macro.text == "FJELL_UI_COMPONENT") kind = ComponentMacro::ui_widget;
        if (macro.text == "FJELL_NOTIFY") kind = ComponentMacro::notify;
        const std::string macro_text = macro.text;
        next();

        std::string name, tag;
        bool well_formed = is("(") && tok(1).kind == TokenKind::identifier;
        if (well_formed) {
            name = tok(1).text;
            if (macro_text == "FJELL_UI_COMPONENT") {
                well_formed = is(",", 2) && tok(3).kind == TokenKind::string && is(")", 4);
                if (well_formed) tag = string_body(tok(3));
            } else {
                well_formed = is(")", 2);
            }
        }
        if (is("(")) skip_balanced();
        if (is(";")) next();

        if (!well_formed) {
            error(line, macro_text == "FJELL_UI_COMPONENT"
                            ? "expected FJELL_UI_COMPONENT(ClassName, \"tag\")"
                            : "expected " + macro_text + "(ClassName)");
            return;
        }
        if (cls.macro != ComponentMacro::none) {
            error(line, "class " + cls.name + " already names itself with " +
                            std::string(cls.macro == ComponentMacro::notify ? "FJELL_NOTIFY" : "a component macro") +
                            " on line " + std::to_string(cls.macro_line));
            return;
        }
        if (name != cls.name) {
            error(line, macro_text + "(" + name + ") sits in class " + cls.name +
                            "; the macro names the class it is in");
            return;
        }
        cls.macro = kind;
        cls.macro_name = name;
        cls.widget_tag = tag;
        cls.macro_line = line;
    }

    // ── Members ─────────────────────────────────────────────────────────

    /// The declaration after `m.attrs`: a data member or a function, in a
    /// class or at namespace level. False when it could not be read.
    bool read_declaration(Member& m) {
        const std::size_t start = pos_;
        std::size_t name_index = std::string::npos;
        int angle = 0;

        // Walk the declaration up to its name: a `(` right after a name opens
        // a function's parameters; `;`, `=`, `{`, `,`, `:` or `[` ends a data
        // member's declarator.
        while (true) {
            if (at_end()) {
                error(tokens_[start].line, "unexpected end of file in a declaration");
                return false;
            }
            if (is("}") && angle == 0) {
                error(tok().line, "expected ';' after the declaration");
                return false;
            }
            if (at_attribute()) {
                for (const auto& a : parse_attributes()) {
                    if (a.scope == "fj") {
                        error(a.line, "[[fj::" + a.name + "]] goes before the declaration it marks");
                    }
                }
                continue;
            }
            if (is("<")) {
                ++angle;
                next();
                continue;
            }
            if (is(">") && angle > 0) {
                --angle;
                next();
                continue;
            }
            if (angle > 0) {
                if (is("(") || is("[") || is("{")) {
                    if (!skip_balanced()) return false;
                } else {
                    next();
                }
                continue;
            }
            if (is("operator")) {
                name_index = pos_;
                next();
                // The operator's own symbol, then its parameter list.
                if (is("(") && is(")", 1)) {
                    next();
                    next();
                }
                while (!at_end() && !is("(") && !is(";")) next();
                if (!is("(")) break;
                m.is_function = true;
                break;
            }
            if (is("decltype") || is("sizeof") || is("alignof") || is("noexcept") ||
                is("__attribute__") || is("__declspec")) {
                next();
                if (is("(")) skip_balanced();
                continue;
            }
            if (is("(")) {
                const bool after_name = pos_ > start && tokens_[pos_ - 1].kind == TokenKind::identifier;
                if (after_name && !is("*", 1) && !is("&", 1) && !is("^", 1)) {
                    name_index = pos_ - 1;
                    m.is_function = true;
                    break;
                }
                m.unsupported_declarator = true;
                if (!skip_balanced()) return false;
                continue;
            }
            if (is(";") || is("=") || is("{") || is(",") || is(":") || is("[")) break;
            next();
        }

        if (m.is_function) {
            parse_function_rest(m, start, name_index);
        } else {
            parse_data_rest(m, start);
        }
        return true;
    }

    /// Specifiers that say how a member is stored or bound, not what it is.
    static bool is_storage_specifier(const Token& t) {
        static const char* words[] = {"static", "mutable", "inline", "constexpr", "consteval",
                                      "constinit", "thread_local", "virtual", "explicit",
                                      "extern", "friend"};
        if (t.kind != TokenKind::identifier) return false;
        return std::any_of(std::begin(words), std::end(words),
                           [&](const char* w) { return t.text == w; });
    }

    /// The type named by tokens [begin, end), storage specifiers left out.
    std::string type_text(std::size_t begin, std::size_t end, bool& is_static) const {
        TokenList kept;
        for (std::size_t i = begin; i < end; ++i) {
            if (is_storage_specifier(tokens_[i])) {
                if (tokens_[i].text == "static") is_static = true;
                continue;
            }
            kept.push_back(tokens_[i]);
        }
        return join(kept, 0, kept.size());
    }

    /// At the token that ends a data member's declarator.
    void parse_data_rest(Member& m, std::size_t start) {
        const std::size_t end = pos_;
        if (end > start && tokens_[end - 1].kind == TokenKind::identifier) {
            m.name = tokens_[end - 1].text;
            m.line = tokens_[end - 1].line;
            m.type = type_text(start, end - 1, m.is_static);
        } else {
            m.line = tokens_[std::min(start, tokens_.size() - 1)].line;
            m.unsupported_declarator = true;
        }

        if (is("[")) {
            m.is_array = true;
            skip_balanced();
        }
        if (is(",")) m.several_declarators = true;
        if (is("{")) {
            skip_balanced();
            if (is(",")) m.several_declarators = true;
        }
        skip_to_semicolon(); // an `= initializer`, a bit-field width, more declarators
    }

    /// At a function's `(`; name_index is the token that names it.
    void parse_function_rest(Member& m, std::size_t start, std::size_t name_index) {
        m.name = tokens_[name_index].text;
        m.line = tokens_[name_index].line;
        std::size_t type_end = name_index;
        if (name_index > start && tokens_[name_index - 1].text == "~") {
            m.name = "~" + m.name;
            --type_end;
        }
        m.type = type_text(start, type_end, m.is_static);

        // Parameters, split at the commas between them.
        const std::size_t open = pos_;
        if (!skip_balanced()) return;
        const std::size_t close = pos_ - 1;
        std::size_t param_begin = open + 1;
        int depth = 0;
        for (std::size_t i = open + 1; i <= close; ++i) {
            const std::string& t = tokens_[i].text;
            const bool punct = tokens_[i].kind == TokenKind::punct;
            if (punct && (t == "(" || t == "[" || t == "{" || t == "<")) ++depth;
            else if (punct && (t == ")" || t == "]" || t == "}" || t == ">") && depth > 0) --depth;
            else if ((punct && t == "," && depth == 0) || i == close) {
                add_parameter(m, param_begin, i);
                param_begin = i + 1;
            }
        }

        // Qualifiers, then `;`, `= 0|default|delete`, a body, or a
        // constructor's initialiser list and then its body.
        while (!at_end() && !is(";") && !is("{") && !is("=") && !is(":") && !is("}")) {
            if (at_attribute()) {
                parse_attributes();
            } else if (is("(")) {
                skip_balanced(); // noexcept(...), throw(...)
            } else {
                if (is("const")) m.is_const = true;
                next();
            }
        }
        if (is("=")) {
            skip_to_semicolon();
            return;
        }
        if (is(":")) {
            next();
            while (!at_end() && !is("{") && !is(";") && !is("}")) {
                while (tok().kind == TokenKind::identifier || is("::")) {
                    next();
                    if (is("<")) skip_angles();
                }
                if (is("(") || is("{")) {
                    if (!skip_balanced()) return;
                }
                if (is(",")) next();
                else break;
            }
        }
        if (is("{")) {
            m.has_body = true;
            skip_balanced();
            if (is(";")) next();
            return;
        }
        if (is(";")) next();
    }

    void add_parameter(Member& m, std::size_t begin, std::size_t end) {
        // A default argument is not part of the parameter's type.
        std::size_t stop = begin;
        int depth = 0;
        while (stop < end) {
            const std::string& t = tokens_[stop].text;
            if (t == "(" || t == "[" || t == "{" || t == "<") ++depth;
            else if ((t == ")" || t == "]" || t == "}" || t == ">") && depth > 0) --depth;
            else if (t == "=" && depth == 0) break;
            ++stop;
        }
        if (stop == begin) return;
        if (stop == begin + 1 && tokens_[begin].text == "void") return; // f(void)

        // The last word names the parameter unless it is part of the type:
        // `glm::vec3` or `int` alone name nothing.
        std::string name;
        std::size_t type_end = stop;
        const Token& last = tokens_[stop - 1];
        if (stop - begin >= 2 && last.kind == TokenKind::identifier &&
            tokens_[stop - 2].text != "::" && !is_type_keyword(last.text)) {
            name = last.text;
            type_end = stop - 1;
        }
        // const and references say how it is passed, not what it is.
        std::size_t type_begin = begin;
        while (type_begin < type_end && tokens_[type_begin].text == "const") ++type_begin;
        while (type_end > type_begin && tokens_[type_end - 1].text == "&") --type_end;
        m.param_types.push_back(join(tokens_, type_begin, type_end));
        m.param_names.push_back(name);
    }

    static bool is_type_keyword(const std::string& word) {
        static const char* words[] = {"int", "float", "double", "bool", "char", "long", "short",
                                      "unsigned", "signed", "void", "auto", "const", "volatile"};
        return std::any_of(std::begin(words), std::end(words),
                           [&](const char* w) { return word == w; });
    }

    // ── What a class contributes ────────────────────────────────────────

    void finish_class(const ClassScope& cls) {
        record_declaration(cls);
        if (cls.macro == ComponentMacro::none) {
            for (const auto& a : cls.attrs) {
                if (a.scope != "fj") continue;
                error(a.line, "[[fj::" + a.name + "]] on class " + cls.name +
                                  ", which is not a component: add FJELL_COMPONENT to it");
                break;
            }
            // A plain struct may be a Model's type; the linker reflects it if
            // one reaches it. Only [[fj::state]] means something there.
            for (const auto& m : cls.members) {
                for (const auto& a : m.attrs) {
                    if (a.scope != "fj" || a.name == "state") continue;
                    error(a.line, "[[fj::" + a.name + "]] on '" + m.name + "' in class " + cls.name +
                                      ", which is not a component: add FJELL_COMPONENT, "
                                      "FJELL_UI_WIDGET or FJELL_NOTIFY to it");
                    break;
                }
            }
            return;
        }

        ComponentInfo comp;
        comp.class_name = cls.macro_name;
        comp.qualified_name = cls.qualified_name;
        comp.source_file = path_;
        comp.is_ui_component = cls.macro == ComponentMacro::ui_widget;
        comp.is_notify = cls.macro == ComponentMacro::notify;
        comp.widget_tag = cls.widget_tag;

        for (const auto& m : cls.members) add_member(comp, m);
        for (const auto& a : cls.attrs) {
            if (a.scope != "fj") continue;
            if (a.name != "tick") {
                error(a.line, "[[fj::" + a.name + "]] does not apply to a class; on a class "
                              "fjreflect knows [[fj::tick(...)]]");
            } else if (cls.macro != ComponentMacro::component) {
                error(a.line, "[[fj::tick]] on " + cls.macro_name + ": only a script component "
                              "(FJELL_COMPONENT) ticks");
            } else {
                apply_tick(comp, a);
            }
        }

        // A component with nothing else to reflect still says how it ticks.
        if (!comp.fields.empty() || !comp.actions.empty() || !comp.rpcs.empty() ||
            comp.is_ui_component || comp.has_tick) {
            result_.components.push_back(std::move(comp));
        }
    }

    /// The class as declared, for the linker.
    void record_declaration(const ClassScope& cls) {
        ClassDecl decl;
        decl.name = cls.name;
        decl.qualified_name = cls.qualified_name;
        decl.source_file = path_;
        decl.line = cls.line;
        decl.is_component = cls.macro != ComponentMacro::none;
        for (const auto& m : cls.members) {
            MemberDecl md;
            md.name = m.name;
            md.type = m.type;
            md.line = m.line;
            md.is_function = m.is_function;
            md.is_static = m.is_static;
            md.is_const = m.is_const;
            md.is_public = m.is_public;
            md.is_readable = !m.is_array && !m.several_declarators && !m.unsupported_declarator && !m.name.empty();
            md.param_types = m.param_types;
            for (const auto& a : m.attrs) {
                if (a.scope == "fj") md.fj_attributes.push_back(a.name);
            }
            decl.members.push_back(std::move(md));
        }
        result_.classes.push_back(std::move(decl));
    }

    static bool is_field_attribute(const std::string& name) {
        static const char* names[] = {"property", "bind", "state", "model", "prop", "range", "step",
                                      "tooltip", "category", "color", "enum", "readonly",
                                      "replicated", "shader_type"};
        return std::any_of(std::begin(names), std::end(names),
                           [&](const char* n) { return name == n; });
    }

    static bool has_attribute(const Member& m, const char* name) {
        return std::any_of(m.attrs.begin(), m.attrs.end(),
                           [&](const Attribute& a) { return a.scope == "fj" && a.name == name; });
    }

    void add_member(ComponentInfo& comp, const Member& m) {
        const Attribute* action = nullptr;
        const Attribute* rpc = nullptr;
        bool field = false;
        bool bad = false;
        for (const auto& a : m.attrs) {
            if (a.scope != "fj") continue;
            if (a.name == "action") action = &a;
            else if (a.name == "rpc") rpc = &a;
            else if (is_field_attribute(a.name)) field = true;
            else {
                error(a.line, "unknown attribute [[fj::" + a.name + "]] on '" + m.name + "'");
                bad = true;
            }
        }
        if (bad) return;

        if (action || rpc) {
            const char* which = action ? "[[fj::action]]" : "[[fj::rpc]]";
            if (field || (action && rpc)) {
                error(m.line, std::string(which) + " cannot be combined with other fj attributes on '" +
                                  m.name + "'");
                return;
            }
            if (!m.is_function) {
                error(m.line, std::string(which) + " marks a method; '" + m.name + "' is not one");
                return;
            }
            if (m.type != "void") {
                error(m.line, std::string(which) + " method '" + m.name + "' must return void");
                return;
            }
            if (action) add_action(comp, m, *action);
            else add_rpc(comp, m, *rpc);
            return;
        }

        if (!field) {
            // Not reflected. A raw pointer to an engine object is worth a word:
            // nothing keeps it valid when the object goes away.
            if (!m.is_function && !m.name.empty() && is_engine_pointer(m.type)) {
                warning(m.line, "untracked raw pointer '" + m.type + " " + m.name + "' to engine type");
            }
            return;
        }
        if (m.is_function && has_attribute(m, "state")) {
            add_getter(comp, m);
            return;
        }
        add_field(comp, m);
    }

    /// `[[fj::state]] bool low_health() const`: a computed value the UI reads
    /// through a generated trampoline.
    void add_getter(ComponentInfo& comp, const Member& m) {
        FieldInfo fi;
        fi.name = m.name;
        fi.line = m.line;
        fi.scope = comp.qualified_name;
        if (!read_getter_type(m, fi)) return;
        for (const auto& a : m.attrs) {
            if (a.scope != "fj" || a.name == "state" || a.name == "tooltip" || a.name == "category") continue;
            error(a.line, "[[fj::" + a.name + "]] cannot go on getter '" + m.name +
                              "'; a getter takes only [[fj::state]], a tooltip and a category");
            return;
        }
        if (!apply_attributes(fi, m.attrs)) return;
        comp.fields.push_back(std::move(fi));
    }

    /// Fills a getter's type; false, after reporting, when `m` cannot be one.
    bool read_getter_type(const Member& m, FieldInfo& fi) {
        if (m.is_static) {
            error(m.line, "getter '" + m.name + "' is static; [[fj::state]] reads a member function");
            return false;
        }
        if (!m.is_const) {
            error(m.line, "getter '" + m.name + "' must be const: the UI calls it every frame and "
                          "must not change what it reads");
            return false;
        }
        if (!m.param_types.empty()) {
            error(m.line, "getter '" + m.name + "' must take no parameters");
            return false;
        }
        fi.cpp_type = value_type_of(m.type);
        std::string ref;
        fi.fj_field_type = map_field_type(fi.cpp_type, ref);
        if (!is_plain_value(fi.fj_field_type)) {
            error(m.line, "getter '" + m.name + "' returns '" + m.type + "'; a getter returns bool, int, "
                          "float, glm::vec2/3/4 or std::string");
            return false;
        }
        fi.is_getter = true;
        fi.is_state = true;
        return true;
    }

    void add_field(ComponentInfo& comp, const Member& m) {
        if (m.is_function) {
            error(m.line, "fj field attributes mark a data member; '" + m.name + "' is a function");
            return;
        }
        if (m.unsupported_declarator || m.name.empty()) {
            error(m.line, "fjreflect cannot read this declaration; declare one field as 'Type name;'");
            return;
        }
        if (m.is_static) {
            error(m.line, "'" + m.name + "' is static; only instance fields can be reflected");
            return;
        }
        if (m.is_array) {
            error(m.line, "'" + m.name + "' is an array, which fjreflect cannot reflect");
            return;
        }
        if (m.several_declarators) {
            error(m.line, "declare one reflected field per declaration ('" + m.name + "')");
            return;
        }

        FieldInfo fi;
        fi.name = m.name;
        fi.cpp_type = m.type;
        fi.line = m.line;
        fi.scope = comp.qualified_name;
        // What [[fj::bind]] did is done by the markup reading the widget in
        // place, and a script no longer holds its widgets.
        if (has_attribute(m, "bind")) {
            error(m.line, "[[fj::bind]] on '" + m.name + "' is gone: mark a field the markup reads "
                          "[[fj::state]] or put it in a [[fj::model]] struct, and read the game from the UI "
                          "through a <model> rather than handing the script a widget");
            return;
        }
        fi.is_state = has_attribute(m, "state");
        fi.is_model = has_attribute(m, "model");
        fi.is_prop = has_attribute(m, "prop");
        if (!apply_attributes(fi, m.attrs)) return;

        if (fi.is_model) {
            if (!comp.is_ui_component) {
                error(m.line, "[[fj::model]] on '" + m.name + "' belongs in a UI widget: a Model is what "
                              "a widget's markup binds");
                return;
            }
            fi.is_state = true;
        }
        if (fi.is_prop) {
            if (!comp.is_ui_component) {
                error(m.line, "[[fj::prop]] on '" + m.name + "' belongs in a UI widget: a prop is what a "
                              "placement passes its widget");
                return;
            }
            if (fi.is_model || fi.is_state || has_attribute(m, "property")) {
                error(m.line, "'" + m.name + "' is filled from a prop ([[fj::prop]]), so it is neither "
                              "authored, a Model nor state of its own");
                return;
            }
            fi.is_state = true; // runtime, never saved
            add_state_field(comp, m, std::move(fi));
            return;
        }
        if (fi.is_state && has_attribute(m, "property")) {
            error(m.line, "'" + m.name + "' is either authored ([[fj::property]]) or runtime state "
                          "([[fj::state]]), not both");
            return;
        }
        if (fi.is_state) {
            add_state_field(comp, m, std::move(fi));
            return;
        }

        if (is_engine_pointer(m.type)) {
            error(m.line, "[[fj::property]] on raw pointer '" + m.type + " " + m.name +
                              "' — use WeakRef<T> or Ref<T>");
            return;
        }

        std::string ref_target;
        fi.fj_field_type = map_field_type(m.type, ref_target);
        fi.ref_target = ref_target;
        if (fi.fj_field_type.empty()) {
            error(m.line, "unsupported type '" + m.type + "' for [[fj::property]] on '" + m.name + "'");
            return;
        }
        if (!finish_value_field(fi, m)) return;
        comp.fields.push_back(std::move(fi));
    }

    /// Runtime state or a Model: a value, a struct or a list. Structs are
    /// resolved by the linker once every header has been read.
    void add_state_field(ComponentInfo& comp, const Member& m, FieldInfo fi) {
        const auto value = classify_value(m.type);
        if (!value.error.empty()) {
            error(m.line, "'" + m.name + "': " + value.error);
            return;
        }
        fi.fj_field_type = value.fj;
        fi.ref_target = value.ref_target;
        fi.struct_type = value.struct_name;
        fi.element_cpp_type = value.element_cpp;
        fi.element_fj_type = value.element_fj;
        fi.element_struct = value.element_struct;
        if (fi.is_model && fi.fj_field_type != "FJELL_FIELD_STRUCT") {
            error(m.line, "[[fj::model]] on '" + m.name + "' needs a struct; '" + m.type + "' is not one");
            return;
        }
        if (fi.is_prop && fi.fj_field_type != "FJELL_FIELD_BOOL" && fi.fj_field_type != "FJELL_FIELD_INT" &&
            fi.fj_field_type != "FJELL_FIELD_FLOAT" && fi.fj_field_type != "FJELL_FIELD_STD_STRING") {
            error(m.line, "[[fj::prop]] '" + m.name + "' holds a bool, int, float or std::string, the kinds "
                          "a <prop> declares; '" + m.type + "' is not one");
            return;
        }
        const bool compound = fi.fj_field_type == "FJELL_FIELD_STRUCT" || fi.fj_field_type == "FJELL_FIELD_VECTOR";
        if (compound && fi.is_replicated) {
            error(m.line, "'" + m.name + "' holds a struct or a list, which cannot be replicated");
            return;
        }
        if (compound && (fi.is_color || !fi.enum_labels.empty())) {
            error(m.line, "[[fj::color]] and [[fj::enum]] need a value, and '" + m.name + "' holds a " +
                              (fi.fj_field_type == "FJELL_FIELD_STRUCT" ? "struct" : "list"));
            return;
        }
        if (!compound && !finish_value_field(fi, m)) return;
        comp.fields.push_back(std::move(fi));
    }

    /// Colour, enum and shader-type checks every value field goes through.
    bool finish_value_field(FieldInfo& fi, const Member& m) {
        if (fi.is_color) {
            if (fi.fj_field_type == "FJELL_FIELD_VEC3") {
                fi.fj_field_type = "FJELL_FIELD_COLOR3";
            } else if (fi.fj_field_type == "FJELL_FIELD_VEC4") {
                fi.fj_field_type = "FJELL_FIELD_COLOR4";
            } else {
                error(m.line, "[[fj::color]] requires vec3 or vec4, got '" + m.type + "' on '" + m.name + "'");
                return false;
            }
        }
        if (!fi.enum_labels.empty()) {
            if (fi.fj_field_type != "FJELL_FIELD_INT") {
                error(m.line, "[[fj::enum]] requires int field, got '" + m.type + "' on '" + m.name + "'");
                return false;
            }
            fi.fj_field_type = "FJELL_FIELD_ENUM";
        }
        if (auto message = shader_type_error(fi); !message.empty()) {
            error(m.line, message);
            return false;
        }
        return true;
    }

    /// `[[fj::pipe]] std::string coins(int copper)` at namespace level.
    void parse_pipe(const std::string& ns, std::vector<Attribute> attrs) {
        Member m;
        m.attrs = std::move(attrs);
        if (!read_declaration(m)) return;
        for (const auto& a : m.attrs) {
            if (a.scope == "fj" && a.name != "pipe") {
                error(a.line, "[[fj::" + a.name + "]] cannot go on pipe '" + m.name + "'");
                return;
            }
            if (a.scope == "fj" && a.has_args) {
                error(a.line, "[[fj::pipe]] takes no arguments; the pipe is named after its function");
                return;
            }
        }
        if (!m.is_function) {
            error(m.line, "[[fj::pipe]] marks a function; '" + m.name + "' is not one");
            return;
        }
        if (value_type_of(m.type) != "std::string") {
            error(m.line, "pipe '" + m.name + "' must return std::string: it formats a value for display");
            return;
        }
        if (m.param_types.empty()) {
            error(m.line, "pipe '" + m.name + "' needs a first parameter: the value it formats");
            return;
        }
        PipeInfo pipe;
        pipe.name = m.name;
        pipe.qualified_name = ns.empty() ? m.name : ns + "::" + m.name;
        pipe.source_file = path_;
        pipe.line = m.line;
        for (std::size_t i = 0; i < m.param_types.size(); ++i) {
            std::string ref;
            const auto type = value_type_of(m.param_types[i]);
            const auto kind = map_field_type(type, ref);
            const bool literal = kind == "FJELL_FIELD_INT" || kind == "FJELL_FIELD_FLOAT" ||
                                 kind == "FJELL_FIELD_STD_STRING";
            if (i == 0 ? !is_plain_value(kind) : !literal) {
                error(m.line, i == 0 ? "pipe '" + m.name + "' formats bool, int, float, glm::vec2/3/4 or "
                                       "std::string, not '" + type + "'"
                                     : "pipe '" + m.name + "': argument " + std::to_string(i) +
                                           " is a literal in markup, so int, float or std::string, not '" +
                                           type + "'");
                return;
            }
            if (i == 0) {
                pipe.input_cpp_type = type;
                pipe.input_fj_type = kind;
            } else {
                pipe.arg_cpp_types.push_back(type);
                pipe.arg_fj_types.push_back(kind);
            }
        }
        result_.pipes.push_back(std::move(pipe));
    }

    // ── Attribute arguments ─────────────────────────────────────────────

    /// The arguments split at their top-level commas.
    static std::vector<TokenList> split_args(const TokenList& args) {
        std::vector<TokenList> out;
        if (args.empty()) return out;
        out.emplace_back();
        int depth = 0;
        for (const auto& t : args) {
            if (t.text == "(" || t.text == "[" || t.text == "{") ++depth;
            else if (t.text == ")" || t.text == "]" || t.text == "}") --depth;
            if (t.kind == TokenKind::punct && t.text == "," && depth == 0) {
                out.emplace_back();
                continue;
            }
            out.back().push_back(t);
        }
        return out;
    }

    /// A number, optionally signed: `-89.0`, `0.5f`, `1'000`.
    static bool read_number(const TokenList& arg, float& out) {
        std::size_t i = 0;
        std::string text;
        if (arg.size() == 2 && (arg[0].text == "-" || arg[0].text == "+")) {
            text = arg[0].text;
            i = 1;
        }
        if (arg.size() != i + 1 || arg[i].kind != TokenKind::number) return false;
        for (char c : arg[i].text) {
            if (c != '\'') text += c;
        }
        while (!text.empty() && (text.back() == 'f' || text.back() == 'F')) text.pop_back();
        try {
            std::size_t used = 0;
            out = std::stof(text, &used);
            return used == text.size();
        } catch (...) {
            return false;
        }
    }

    /// One or more adjacent string literals, joined.
    static bool read_string(const TokenList& arg, std::string& out) {
        if (arg.empty()) return false;
        out.clear();
        for (const auto& t : arg) {
            if (t.kind != TokenKind::string) return false;
            out += string_body(t);
        }
        return true;
    }

    /// A name, qualified or not, or a string: the last name, or the string.
    static bool read_word(const TokenList& arg, std::string& out) {
        if (arg.size() == 1 && arg[0].kind == TokenKind::string) {
            out = string_body(arg[0]);
            return true;
        }
        if (arg.empty() || arg.back().kind != TokenKind::identifier) return false;
        for (std::size_t i = 0; i < arg.size(); ++i) {
            const bool name_slot = i % 2 == 0;
            if (name_slot ? arg[i].kind != TokenKind::identifier : arg[i].text != "::") return false;
        }
        out = arg.back().text;
        return true;
    }

    /// The keys of [[fj::tick(...)]] on a script component: `order = n`, a
    /// whole number, lower running earlier. An error on anything else.
    void apply_tick(ComponentInfo& comp, const Attribute& a) {
        const std::string what = "[[fj::tick]] on '" + comp.class_name + "'";
        if (!a.has_args) {
            error(a.line, what + " takes keys, as fj::tick(order = 10)");
            return;
        }
        for (const TokenList& arg : split_args(a.args)) {
            if (arg.size() < 3 || arg[0].kind != TokenKind::identifier || arg[1].text != "=") {
                error(a.line, what + " takes key = value pairs, as fj::tick(order = 10)");
                return;
            }
            const std::string& key = arg[0].text;
            const TokenList value(arg.begin() + 2, arg.end());
            if (key == "order") {
                float order = 0.0f;
                if (!read_number(value, order) || order != std::floor(order)) {
                    error(a.line, what + ": order is a whole number");
                    return;
                }
                comp.tick_order = static_cast<int>(order);
            } else {
                error(a.line, what + " has no key '" + key + "'; it takes: order");
                return;
            }
        }
        comp.has_tick = true;
    }

    /// Hints and decorations onto a field. False, with an error, when an
    /// attribute's arguments are not what it takes.
    bool apply_attributes(FieldInfo& fi, const std::vector<Attribute>& attrs) {
        for (const auto& a : attrs) {
            if (a.scope != "fj") continue;
            const auto args = split_args(a.args);
            auto bad = [&](const std::string& expected) {
                error(a.line, "[[fj::" + a.name + "]] on '" + fi.name + "' expects " + expected);
                return false;
            };
            const bool takes_none = a.name == "property" || a.name == "bind" || a.name == "state" ||
                                    a.name == "model" || a.name == "prop" || a.name == "color" || a.name == "readonly" ||
                                    a.name == "replicated";
            if (takes_none) {
                if (a.has_args) return bad("no arguments");
                if (a.name == "color") fi.is_color = true;
                if (a.name == "readonly") fi.is_readonly = true;
                if (a.name == "replicated") fi.is_replicated = true;
            } else if (a.name == "range") {
                if (args.size() != 2 || !read_number(args[0], fi.hint_min) ||
                    !read_number(args[1], fi.hint_max)) {
                    return bad("(min, max)");
                }
                fi.has_range = true;
            } else if (a.name == "step") {
                if (args.size() != 1 || !read_number(args[0], fi.hint_speed)) return bad("(step)");
            } else if (a.name == "tooltip" || a.name == "category") {
                auto& text = a.name == "tooltip" ? fi.tooltip : fi.category;
                if (args.size() != 1 || !read_string(args[0], text)) return bad("(\"text\")");
            } else if (a.name == "enum" || a.name == "shader_type") {
                auto& list = a.name == "enum" ? fi.enum_labels : fi.shader_types;
                list.clear();
                for (const auto& arg : args) {
                    std::string word;
                    if (!read_word(arg, word) || word.empty()) return bad("a list of names");
                    list.push_back(word);
                }
                if (list.empty()) return bad("a list of names");
            }
        }
        return true;
    }

    void add_action(ComponentInfo& comp, const Member& m, const Attribute& a) {
        if (!comp.is_ui_component) {
            error(a.line, "[[fj::action]] on '" + m.name + "' belongs in a UI widget; a script "
                          "component binds input with InputAPI::bind() in begin_play");
            return;
        }
        ActionInfo ai;
        ai.method_name = m.name;
        ai.name = m.name;
        if (a.has_args) {
            const auto args = split_args(a.args);
            std::string trigger;
            if (args.size() != 1 || !read_word(args[0], trigger)) {
                error(a.line, "[[fj::action]] on '" + m.name + "' expects (pressed), (released), "
                              "(completed) or (triggered)");
                return;
            }
            trigger = lower(trigger);
            if (trigger == "pressed") ai.trigger = ActionTriggerMode::pressed;
            else if (trigger == "released") ai.trigger = ActionTriggerMode::released;
            else if (trigger == "completed") ai.trigger = ActionTriggerMode::completed;
            else if (trigger == "triggered") ai.trigger = ActionTriggerMode::triggered;
            else {
                error(a.line, "unknown trigger '" + trigger + "' in [[fj::action]] on '" + m.name +
                                  "' (pressed, released, completed, triggered)");
                return;
            }
        }
        if (m.param_types.size() > 1) {
            error(m.line, "[[fj::action]] method '" + m.name + "' takes at most one parameter");
            return;
        }
        if (m.param_types.size() == 1) {
            auto type = m.param_types[0];
            if (type.starts_with("fjell::")) type = type.substr(7);
            if (type == "int" || type == "int32_t") {
                ai.param = ActionParamType::int_param;
            } else if (type == "float") {
                ai.param = ActionParamType::float_param;
            } else if (type == "glm::vec2" || type == "vec2") {
                ai.param = ActionParamType::vec2_param;
            } else {
                error(m.line, "[[fj::action]] method '" + m.name + "' takes int, float or glm::vec2, not '" +
                                  m.param_types[0] + "'");
                return;
            }
        }
        comp.actions.push_back(std::move(ai));
    }

    void add_rpc(ComponentInfo& comp, const Member& m, const Attribute& a) {
        if (m.has_body) {
            error(m.line, "[[fj::rpc]] method '" + m.name + "' is defined by fjreflect; declare it "
                          "without a body and write the handler as " + m.name + "_impl");
            return;
        }
        RpcInfo ri;
        ri.method_name = m.name;
        if (a.has_args) {
            const auto args = split_args(a.args);
            std::string target;
            if (args.size() != 1 || !read_word(args[0], target)) {
                error(a.line, "[[fj::rpc]] on '" + m.name + "' expects (server), (clients) or (all)");
                return;
            }
            target = lower(target);
            if (target == "server") ri.target = RpcTarget::server;
            else if (target == "clients") ri.target = RpcTarget::clients;
            else if (target == "all") ri.target = RpcTarget::all;
            else {
                error(a.line, "unknown target '" + target + "' in [[fj::rpc]] on '" + m.name +
                                  "' (server, clients, all)");
                return;
            }
        }
        for (std::size_t p = 0; p < m.param_types.size(); ++p) {
            RpcParam param;
            param.cpp_type = m.param_types[p];
            param.name = m.param_names[p].empty() ? "p" + std::to_string(p) : m.param_names[p];
            ri.params.push_back(std::move(param));
        }
        comp.rpcs.push_back(std::move(ri));
    }

    std::string path_;
    TokenList tokens_;
    ParseResult& result_;
    std::size_t pos_{0};
};

} // namespace

ParseResult parse_file(const std::string& filepath) {
    ParseResult result;

    std::ifstream file(filepath, std::ios::binary);
    if (!file) {
        result.errors.push_back(filepath + ": cannot open file");
        return result;
    }
    std::ostringstream text;
    text << file.rdbuf();

    auto lexed = tokenize(text.str());
    for (const auto& e : lexed.errors) {
        const auto colon = e.find(':');
        result.errors.push_back(filepath + ":" + e.substr(0, colon) + ": error:" + e.substr(colon + 1));
    }
    if (!lexed.errors.empty()) return result;

    HeaderParser(filepath, std::move(lexed.tokens), result).run();
    return result;
}

} // namespace fjreflect
