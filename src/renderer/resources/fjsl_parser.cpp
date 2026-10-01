#include "renderer/resources/fjsl_parser.hpp"
#include "ftk/base/log.hpp"
#include "ftk/math/color_space.hpp"
#include "core/named_value.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace fjell {

// The keyword ↔ ShaderType mapping, and the only place either spelling is
// written down. Adding a type is this table plus the switches the compiler
// then points at.
namespace {
struct ShaderTypeName {
    std::string_view name;
    ShaderType type;
};
constexpr ShaderTypeName SHADER_TYPE_NAMES[] = {
    {"spatial", ShaderType::Spatial}, {"sky", ShaderType::Sky},
    {"ui", ShaderType::Ui},           {"terrain", ShaderType::Terrain},
    {"water", ShaderType::Water},     {"fog", ShaderType::Fog},
    {"post", ShaderType::Post},
};
} // namespace

std::string_view shader_type_name(ShaderType type) {
    for (const auto& row : SHADER_TYPE_NAMES) {
        if (row.type == type) return row.name;
    }
    return "spatial";
}

// Which hooks each kind of file may define, and why the others are refused.
//
// One table for two jobs that were drifting apart: a hook a file cannot run is
// an error, and a hook it *can* run has its body extracted and must not be left
// behind in the file-scope globals. Stating what is permitted keeps those two
// answers from disagreeing — the previous form listed exclusions for one and a
// fixed five for the other.
namespace {
struct HookRule {
    std::string_view hook;
    std::string_view refusal; ///< empty when this kind permits the hook
};

constexpr std::string_view DENSITY_REFUSAL = "only a shader_type fog shader writes a medium";
constexpr std::string_view COLOR_REFUSAL =
    "only a shader_type post shader writes the display colour";

constexpr HookRule SPATIAL_HOOKS[] = {
    {"fragment", {}}, {"vertex", {}}, {"light", {}},
    {"sky", "only a shader_type sky shader writes SKY"},
    {"layers", "only terrain blends painted layers"},
    {"density", DENSITY_REFUSAL},
    {"color", COLOR_REFUSAL},
};
constexpr HookRule SKY_HOOKS[] = {
    {"sky", {}},
    {"fragment", "a sky shader writes SKY from sky(), not ALBEDO from fragment()"},
    {"vertex", "the sky is drawn as one fullscreen triangle"},
    {"light", "the sky is not lit; it is the light"},
    {"layers", "only terrain blends painted layers"},
    {"density", DENSITY_REFUSAL},
    {"color", COLOR_REFUSAL},
};
constexpr HookRule UI_HOOKS[] = {
    {"fragment", {}},
    {"vertex", "a ui quad has no geometry to displace"},
    {"light", "a ui quad is not lit"},
    {"sky", "only a shader_type sky shader writes SKY"},
    {"layers", "only terrain blends painted layers"},
    {"density", DENSITY_REFUSAL},
    {"color", COLOR_REFUSAL},
};
constexpr HookRule TERRAIN_HOOKS[] = {
    {"fragment", {}}, {"light", {}}, {"layers", {}},
    {"vertex", "terrain geometry is generated in a mesh shader from the height texture"},
    {"sky", "only a shader_type sky shader writes SKY"},
    {"density", DENSITY_REFUSAL},
    {"color", COLOR_REFUSAL},
};
constexpr HookRule WATER_HOOKS[] = {
    {"fragment", {}}, {"light", {}},
    {"vertex", "the surface comes from the .fjwater wave field"},
    {"sky", "only a shader_type sky shader writes SKY"},
    {"layers", "only terrain blends painted layers"},
    {"density", DENSITY_REFUSAL},
    {"color", COLOR_REFUSAL},
};
// A fog shader answers what the medium is at a world point and how it
// scatters each light. It has no surface, so nothing for fragment() to
// shade or vertex() to move.
constexpr HookRule FOG_HOOKS[] = {
    {"density", {}}, {"light", {}},
    {"fragment", "a fog shader writes a medium from density(), not a surface from fragment()"},
    {"vertex", "fog has no geometry; it is sampled per froxel"},
    {"sky", "only a shader_type sky shader writes SKY"},
    {"layers", "only terrain blends painted layers"},
    {"color", COLOR_REFUSAL},
};
// A post shader takes the finished frame to the display: COLOR in, exposed,
// COLOR out, display-linear. The frame is already lit and has no geometry
// left to move.
constexpr HookRule POST_HOOKS[] = {
    {"color", {}},
    {"fragment", "a post shader writes COLOR from color(), not a surface from fragment()"},
    {"vertex", "a post shader is drawn as one fullscreen triangle"},
    {"light", "a post shader works on the lit frame; nothing in it is lit again"},
    {"sky", "only a shader_type sky shader writes SKY"},
    {"layers", "only terrain blends painted layers"},
    {"density", DENSITY_REFUSAL},
};

std::span<const HookRule> hook_rules(ShaderType type) {
    switch (type) {
        case ShaderType::Sky:     return SKY_HOOKS;
        case ShaderType::Ui:      return UI_HOOKS;
        case ShaderType::Terrain: return TERRAIN_HOOKS;
        case ShaderType::Water:   return WATER_HOOKS;
        case ShaderType::Fog:     return FOG_HOOKS;
        case ShaderType::Post:    return POST_HOOKS;
        case ShaderType::Spatial: break;
    }
    return SPATIAL_HOOKS;
}
} // namespace

std::optional<ShaderType> shader_type_from_name(std::string_view name) {
    for (const auto& row : SHADER_TYPE_NAMES) {
        if (row.name == name) return row.type;
    }
    return std::nullopt;
}

std::string_view glsl_type_name(UniformType type) {
    switch (type) {
        case UniformType::Float: return "float";
        case UniformType::Int: return "int";
        case UniformType::Vec2: return "vec2";
        case UniformType::Vec3: return "vec3";
        case UniformType::Vec4: return "vec4";
        case UniformType::Sampler2D: return "sampler2D";
    }
    return "float";
}

uint32_t uniform_float_count(UniformType type) {
    switch (type) {
        case UniformType::Float: return 1;
        case UniformType::Int: return 1;
        case UniformType::Vec2: return 2;
        case UniformType::Vec3: return 3;
        case UniformType::Vec4: return 4;
        case UniformType::Sampler2D: return 0;
    }
    return 0;
}

namespace {

struct SourcePosition {
    uint32_t line{1};   // 1-based
    uint32_t column{1}; // 1-based, in bytes
};

// Line and column of a byte offset, counted on demand.
//
// Counting from the start of the file for every report is a scan per
// diagnostic, which is quadratic in a file that produces many of them. The
// cursor remembers the last offset it reached so a parse that reports in
// increasing order walks the source once in total; an offset behind the
// cursor simply restarts, which keeps the answer right whatever order the
// caller asks in.
struct PositionCursor {
    const std::string& source;
    size_t offset{0};
    SourcePosition pos{};

    explicit PositionCursor(const std::string& src) : source(src) {}

    SourcePosition at(size_t target) {
        const size_t end = std::min(target, source.size());
        if (end < offset) {
            offset = 0;
            pos = {};
        }
        for (; offset < end; ++offset) {
            if (source[offset] == '\n') {
                ++pos.line;
                pos.column = 1;
            } else {
                ++pos.column;
            }
        }
        return pos;
    }
};

bool is_identifier_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// How long the comment starting at `at` is, or 0 when none starts there.
size_t comment_length(std::string_view text, size_t at) {
    if (at + 1 >= text.size() || text[at] != '/') return 0;
    if (text[at + 1] == '/') {
        const size_t end = text.find('\n', at);
        return (end == std::string_view::npos ? text.size() : end) - at;
    }
    if (text[at + 1] == '*') {
        const size_t end = text.find("*/", at + 2);
        return (end == std::string_view::npos ? text.size() : end + 2) - at;
    }
    return 0;
}

// `source` with every comment blanked to spaces, its newlines kept, so every
// offset still names the same character. Searching this rather than the
// source keeps a comment that says "before fragment() runs" from being taken
// for the hook, and a brace in a comment from ending a body.
std::string mask_comments(const std::string& source) {
    std::string code = source;
    for (size_t at = 0; at < code.size();) {
        const size_t length = comment_length(code, at);
        if (length == 0) {
            ++at;
            continue;
        }
        for (size_t i = at; i < at + length; ++i) {
            if (code[i] != '\n') code[i] = ' ';
        }
        at += length;
    }
    return code;
}

// ── Which names in the shader's own code are its uniforms ───────────────────
//
// A uniform is read through a function the generator emits for it, and the
// shader's own code is rewritten to call that function wherever a name means
// the uniform. Where it does is decided by GLSL's own scoping, so a uniform's
// name never reaches anything but the author's code: not a file it includes,
// not the template, not a struct field or a parameter that happens to share it.

using UniformNames = std::set<std::string, std::less<>>;

// One place the code names a uniform.
struct UniformReference {
    size_t offset{0};
    std::string name;
};

struct UniformScan {
    /// Where the code reads a uniform, in order.
    std::vector<UniformReference> reads;
    /// The reads that assign to it, or increment or decrement it.
    std::vector<UniformReference> writes;
    /// A uniform's name declared again at file scope.
    std::vector<UniformReference> redeclared;
};

struct GlslToken {
    enum class Kind : uint8_t { Word, Number, Punct };
    Kind kind{Kind::Punct};
    size_t begin{0};
    size_t end{0};
};

// A #define's body, which is code like any other, and the names its
// parameters give inside it.
struct MacroBody {
    size_t begin{0};
    size_t end{0};
    std::vector<std::string> parameters;
};

// The operators longer than one character, longest first so `<<=` is not
// read as `<<` and `=`.
constexpr std::string_view LONG_OPERATORS[] = {
    "<<=", ">>=", "++", "--", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=",
    "==", "!=", "<=", ">=", "&&", "||", "^^", "<<", ">>",
};

// The tokens of `code[begin, end)`, whose comments are already blanked:
// words, numbers and operators. A string, which FJSL has (`group "Sun"`) and
// GLSL does not, is skipped, and so is every preprocessor line with its
// continuations; a #define's body is handed to `macros` to be read on its own.
std::vector<GlslToken> tokenize_glsl(std::string_view code, size_t begin, size_t end,
                                     std::vector<MacroBody>* macros) {
    std::vector<GlslToken> tokens;
    bool line_start = true;
    size_t i = begin;
    auto word_end = [&](size_t at) {
        while (at < end && is_identifier_char(code[at])) ++at;
        return at;
    };
    while (i < end) {
        const char c = code[i];
        if (c == '\n') {
            line_start = true;
            ++i;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            ++i;
            continue;
        }
        if (c == '#' && line_start) {
            size_t line_end = i;
            while (line_end < end
                   && !(code[line_end] == '\n' && code[line_end - 1] != '\\')) {
                ++line_end;
            }
            size_t at = i + 1;
            while (at < line_end && (code[at] == ' ' || code[at] == '\t')) ++at;
            const size_t directive_end = word_end(at);
            if (macros != nullptr && code.substr(at, directive_end - at) == "define") {
                at = directive_end;
                while (at < line_end && (code[at] == ' ' || code[at] == '\t')) ++at;
                MacroBody macro;
                at = word_end(at);
                if (at < line_end && code[at] == '(') {
                    while (at < line_end && code[at] != ')') {
                        if (is_identifier_char(code[at])) {
                            const size_t name_end = word_end(at);
                            macro.parameters.emplace_back(code.substr(at, name_end - at));
                            at = name_end;
                        } else {
                            ++at;
                        }
                    }
                    if (at < line_end) ++at;
                }
                macro.begin = at;
                macro.end = line_end;
                macros->push_back(std::move(macro));
            }
            i = line_end;
            continue;
        }
        line_start = false;
        if (c == '"') {
            ++i;
            while (i < end && code[i] != '"' && code[i] != '\n') ++i;
            if (i < end && code[i] == '"') ++i;
            continue;
        }
        const bool digit = std::isdigit(static_cast<unsigned char>(c)) != 0;
        if (digit || (c == '.' && i + 1 < end
                      && std::isdigit(static_cast<unsigned char>(code[i + 1])) != 0)) {
            const size_t start = i;
            ++i;
            while (i < end && (is_identifier_char(code[i]) || code[i] == '.'
                               || ((code[i] == '+' || code[i] == '-')
                                   && (code[i - 1] == 'e' || code[i - 1] == 'E')))) {
                ++i;
            }
            tokens.push_back({GlslToken::Kind::Number, start, i});
            continue;
        }
        if (is_identifier_char(c)) {
            tokens.push_back({GlslToken::Kind::Word, i, word_end(i)});
            i = tokens.back().end;
            continue;
        }
        size_t length = 1;
        for (std::string_view op : LONG_OPERATORS) {
            if (code.substr(i, op.size()) == op) {
                length = op.size();
                break;
            }
        }
        tokens.push_back({GlslToken::Kind::Punct, i, i + length});
        i += length;
    }
    return tokens;
}

// A word after which the next one is not being declared.
bool is_statement_keyword(std::string_view word) {
    return word == "return" || word == "else" || word == "case" || word == "do"
        || word == "default" || word == "break" || word == "continue" || word == "discard";
}

// FJSL's own statements, written at file scope and not GLSL: a uniform's
// declaration names the uniform itself, and the rest name nothing code reads.
// A leftover `use` is among them so the error reported for it is the only one.
bool is_fjsl_statement(std::string_view word) {
    return word == "uniform" || word == "shader_type" || word == "render_mode"
        || word == "group" || word == "use";
}

bool is_assignment(std::string_view op) {
    return op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=" || op == "%="
        || op == "&=" || op == "|=" || op == "^=" || op == "<<=" || op == ">>=";
}

// Walks `tokens` with GLSL's scopes: braces open one, a parameter list or a
// for's header declares into the scope its `{` opens, and a declaration holds
// to the end of its scope. A name is a uniform's where none of that declares
// it, it is not a member (after a `.`) and it is not called (before a `(`).
// `in_function` is true for a hook's body, whose outermost declarations are
// locals; `declared` are names already in scope, a macro's parameters.
void scan_tokens(std::string_view code, const std::vector<GlslToken>& tokens,
                 const UniformNames& uniforms, bool in_function,
                 const std::vector<std::string>& declared, UniformScan& out) {
    using Scope = std::vector<std::string_view>;
    const size_t outermost = in_function ? 2 : 1;
    std::vector<Scope> scopes(outermost);
    for (const auto& name : declared) scopes.back().push_back(name);
    // A function's parameters or a for's variables: in scope from the `{`
    // that follows, or to the end of the statement when no brace does.
    Scope pending;
    int depth = 0;  // parentheses and brackets
    bool declaring = false;
    int declaring_depth = 0;
    bool next_declares = false;
    bool skipping = false;

    const size_t count = tokens.size();
    auto text = [&](size_t k) {
        return code.substr(tokens[k].begin, tokens[k].end - tokens[k].begin);
    };
    auto punct = [&](size_t k, std::string_view p) {
        return k < count && tokens[k].kind == GlslToken::Kind::Punct && text(k) == p;
    };
    auto in_scope = [&](std::string_view name) {
        for (const auto& scope : scopes) {
            if (std::find(scope.begin(), scope.end(), name) != scope.end()) return true;
        }
        return std::find(pending.begin(), pending.end(), name) != pending.end();
    };
    // Whether the read at `k` is written: a member or an element of it may
    // come between the name and the operator.
    auto written = [&](size_t k) {
        if (k > 0 && (punct(k - 1, "++") || punct(k - 1, "--"))) return true;
        size_t j = k + 1;
        while (j < count) {
            if (punct(j, ".") && j + 1 < count && tokens[j + 1].kind == GlslToken::Kind::Word) {
                j += 2;
            } else if (punct(j, "[")) {
                int open = 0;
                for (; j < count; ++j) {
                    if (punct(j, "[")) ++open;
                    if (punct(j, "]") && --open == 0) break;
                }
                ++j;
            } else {
                break;
            }
        }
        return j < count && tokens[j].kind == GlslToken::Kind::Punct
            && (is_assignment(text(j)) || text(j) == "++" || text(j) == "--");
    };

    for (size_t k = 0; k < count; ++k) {
        const std::string_view word = text(k);
        if (tokens[k].kind == GlslToken::Kind::Punct) {
            if (skipping) {
                if (word == ";") skipping = false;
            } else if (word == "(" || word == "[") {
                ++depth;
            } else if (word == ")" || word == "]") {
                depth = std::max(0, depth - 1);
            } else if (word == "{") {
                scopes.push_back(std::move(pending));
                pending.clear();
                declaring = false;
                next_declares = false;
            } else if (word == "}") {
                if (scopes.size() > outermost) scopes.pop_back();
                declaring = false;
            } else if (word == ";" && depth == 0) {
                pending.clear();
                declaring = false;
                next_declares = false;
            } else if (word == "," && declaring && depth == declaring_depth) {
                next_declares = true;
            }
            continue;
        }
        if (tokens[k].kind != GlslToken::Kind::Word || skipping) continue;

        const bool statement_start = k == 0 || punct(k - 1, ";") || punct(k - 1, "{")
                                  || punct(k - 1, "}");
        if (statement_start && scopes.size() == 1 && depth == 0 && is_fjsl_statement(word)) {
            skipping = true;
            continue;
        }
        if (k > 0 && punct(k - 1, ".")) continue;

        const bool call = punct(k + 1, "(");
        const bool after_type =
            k > 0 && ((tokens[k - 1].kind == GlslToken::Kind::Word
                       && !is_statement_keyword(text(k - 1)))
                      || punct(k - 1, "]"));
        if (next_declares || after_type) {
            next_declares = false;
            // A function's own name: it is called, never read.
            if (call) continue;
            (depth > 0 ? pending : scopes.back()).push_back(word);
            declaring = true;
            declaring_depth = depth;
            if (scopes.size() == 1 && depth == 0 && uniforms.contains(word)) {
                out.redeclared.push_back({tokens[k].begin, std::string(word)});
            }
            continue;
        }
        if (call || !uniforms.contains(word) || in_scope(word)) continue;
        out.reads.push_back({tokens[k].begin, std::string(word)});
        if (written(k)) out.writes.push_back(out.reads.back());
    }
}

// Every place `code` names one of `uniforms`; `code` has its comments blanked.
UniformScan scan_uniform_names(std::string_view code, const UniformNames& uniforms,
                               bool in_function) {
    UniformScan scan;
    if (uniforms.empty()) return scan;
    std::vector<MacroBody> macros;
    const auto tokens = tokenize_glsl(code, 0, code.size(), &macros);
    scan_tokens(code, tokens, uniforms, in_function, {}, scan);
    // A macro is expanded where it is used, but what it names is decided
    // where it is written: a uniform, unless the macro's own parameter.
    for (const auto& macro : macros) {
        const auto body = tokenize_glsl(code, macro.begin, macro.end, nullptr);
        scan_tokens(code, body, uniforms, true, macro.parameters, scan);
    }
    std::sort(scan.reads.begin(), scan.reads.end(),
              [](const auto& a, const auto& b) { return a.offset < b.offset; });
    return scan;
}

// The function the generator emits for reading uniform `name`.
std::string uniform_accessor(std::string_view name) {
    return "fjell_uniform_" + std::string(name);
}

// `text`, the shader's own code, with every read of one of `uniforms` turned
// into a call of the function that reads it. `in_function` is true for a
// hook's body.
std::string resolve_uniform_reads(const std::string& text, const UniformNames& uniforms,
                                  bool in_function) {
    const UniformScan scan = scan_uniform_names(mask_comments(text), uniforms, in_function);
    if (scan.reads.empty()) return text;
    std::string out;
    out.reserve(text.size() + scan.reads.size() * 16);
    size_t copied = 0;
    for (const auto& read : scan.reads) {
        out.append(text, copied, read.offset - copied);
        out += uniform_accessor(read.name) + "()";
        copied = read.offset + read.name.size();
    }
    out.append(text, copied, std::string::npos);
    return out;
}

// Offset of `void <name> (` written at an identifier boundary, or npos.
//
// This is the hand-written form of `\bvoid\s+<name>\s*\(`. std::regex costs
// tens of nanoseconds per byte even when it matches nothing, and this runs
// once per forbidden-hook rule over the whole source; a plain scan finds the
// same declarations for a fraction of the work.
size_t find_function_declaration(const std::string& source, std::string_view name) {
    for (size_t pos = source.find(name); pos != std::string::npos;
         pos = source.find(name, pos + 1)) {
        // `\bvoid\s+` before the name, with at least one space.
        size_t before = pos;
        while (before > 0 && std::isspace(static_cast<unsigned char>(source[before - 1]))) --before;
        if (before == pos) continue; // no whitespace separating void from the name
        constexpr std::string_view KEYWORD = "void";
        if (before < KEYWORD.size()) continue;
        const size_t kw = before - KEYWORD.size();
        if (source.compare(kw, KEYWORD.size(), KEYWORD) != 0) continue;
        if (kw > 0 && is_identifier_char(source[kw - 1])) continue;

        // `\s*\(` after it, and the name itself must end at a boundary.
        size_t after = pos + name.size();
        if (after < source.size() && is_identifier_char(source[after])) continue;
        while (after < source.size() && std::isspace(static_cast<unsigned char>(source[after]))) ++after;
        if (after < source.size() && source[after] == '(') return kw;
    }
    return std::string::npos;
}

// Only a colour vector is authored in sRGB; a hint_color on any other type
// is meaningless and left alone.
bool is_authored_color(const FjslUniform& u) {
    return u.hint == UniformHint::Color
        && (u.type == UniformType::Vec3 || u.type == UniformType::Vec4);
}

} // namespace

void encode_uniform_for_gpu(const FjslUniform& u, std::span<float> values) {
    if (!is_authored_color(u)) return;
    const size_t rgb = std::min<size_t>(values.size(), 3);
    for (size_t i = 0; i < rgb; ++i) {
        values[i] = ftk::color_space::srgb_to_linear(values[i]);
    }
}

void decode_uniform_from_gpu(const FjslUniform& u, std::span<float> values) {
    if (!is_authored_color(u)) return;
    const size_t rgb = std::min<size_t>(values.size(), 3);
    for (size_t i = 0; i < rgb; ++i) {
        values[i] = ftk::color_space::linear_to_srgb(values[i]);
    }
}

std::vector<float> resolve_uniform_block(const ShaderMetadata& meta,
                                         const std::vector<NamedValue>& values) {
    std::vector<float> block(meta.custom_param_count, 0.0f);
    for (const auto& u : meta.uniforms) {
        if (u.type == UniformType::Sampler2D) continue;
        const uint32_t count = uniform_float_count(u.type);
        if (u.slot + count > block.size()) continue;
        for (uint32_t i = 0; i < count && i < u.default_value.size(); ++i) {
            block[u.slot + i] = u.default_value[i];
        }
        if (const NamedValue* value = find_named(values, u.name)) {
            for (uint32_t i = 0; i < count && i < value->count; ++i) {
                block[u.slot + i] = value->values[i];
            }
        }
        encode_uniform_for_gpu(u, std::span{block}.subspan(u.slot, count));
    }
    return block;
}

static UniformType parse_type(const std::string& t) {
    if (t == "float") return UniformType::Float;
    if (t == "int") return UniformType::Int;
    if (t == "vec2") return UniformType::Vec2;
    if (t == "vec3") return UniformType::Vec3;
    if (t == "vec4") return UniformType::Vec4;
    if (t == "sampler2D") return UniformType::Sampler2D;
    throw std::runtime_error("Unknown FJSL uniform type: " + t);
}

static UniformHint parse_hint_name(const std::string& h) {
    if (h == "hint_color") return UniformHint::Color;
    if (h == "hint_white") return UniformHint::White;
    if (h == "hint_black") return UniformHint::Black;
    if (h == "hint_normal") return UniformHint::Normal;
    if (h == "hint_default_mr") return UniformHint::DefaultMr;
    if (h == "hint_lut") return UniformHint::Lut;
    return UniformHint::None;
}

// One declaration grammar shared by the metadata pass and the strip pass, so
// a form one of them accepts is never left behind by the other in the
// generated GLSL.
//   uniform <type> <name> [: <hint>[, <hint>...]] [= <default>];
// Groups: 1 type, 2 name, 3 hint list, 4 default.
/// `group "Name";` — a heading the uniforms after it are shown under.
static const std::regex& group_declaration_re() {
    static const std::regex re(R"RX(group\s*"([^"]*)"\s*;)RX");
    return re;
}

static const std::regex& uniform_declaration_re() {
    static const std::regex re(
        R"(uniform\s+(\w+)\s+(\w+)\s*(?::\s*([^=;]+?))?\s*(?:=\s*([^;]+))?\s*;)");
    return re;
}

/// `shader_function;` — what a shared function's file declared before shared
/// code became plain GLSL; found so it can be reported where it is.
static const std::regex& shader_function_re() {
    static const std::regex re(R"(shader_function\s*;)");
    return re;
}

/// `shader_type <word>;` — what a pass-drawn file is. Group 1: the word.
static const std::regex& shader_type_re() {
    static const std::regex re(R"(shader_type\s+(\w+)\s*;)");
    return re;
}

/// `use <name> = "<path>"(` — the head of a composition, which FJSL no longer
/// has; found so it can be reported where it is.
static const std::regex& use_head_re() {
    static const std::regex re(R"RX(use\s+(\w+)\s*=\s*"([^"]*)"\s*\()RX");
    return re;
}

static std::string trim(const std::string& str) {
    auto s = str.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) return {};
    auto e = str.find_last_not_of(" \t\r\n");
    return str.substr(s, e - s + 1);
}

// A number the author wrote, or nothing if it is not one.
//
// std::stof throws on anything unparseable, and the throw used to escape all
// the way out of the parse — so `uniform float x = abc;` reached the user as
// the word "stof" and nothing else. Failing to a nullopt lets the caller say
// which uniform and what it was trying to read.
static std::optional<float> parse_float(const std::string& text) {
    try {
        size_t consumed = 0;
        const float value = std::stof(text, &consumed);
        // Trailing characters mean this was not a number: "1.0abc" would
        // otherwise read as 1.0 and quietly lose the rest.
        if (trim(text.substr(consumed)).empty()) return value;
    } catch (const std::exception&) {
        // fall through
    }
    return std::nullopt;
}

// Split "hint_range(0.0, 1.0, 0.01), hint_emission" on the commas that
// separate hints, not the ones inside a hint's argument list.
static std::vector<std::string> split_hint_list(const std::string& list) {
    std::vector<std::string> out;
    std::string current;
    int depth = 0;
    for (char c : list) {
        if (c == '(') ++depth;
        else if (c == ')') --depth;
        if (c == ',' && depth == 0) {
            out.push_back(trim(current));
            current.clear();
            continue;
        }
        current += c;
    }
    auto last = trim(current);
    if (!last.empty()) out.push_back(last);
    return out;
}

// Problems are handed back rather than logged, so the caller can attach the
// declaration's position: an unlocated warning is one a linter cannot point at
// and an editor cannot underline.
static void apply_hint(FjslUniform& u, const std::string& token,
                       std::vector<std::string>& problems) {
    if (token.rfind("hint_range", 0) == 0) {
        // hint_range(min, max[, step]) — stored beside the UI hint rather
        // than as one, since a ranged float has no picker to choose.
        auto open = token.find('(');
        auto close = token.rfind(')');
        if (open == std::string::npos || close == std::string::npos || close < open) {
            problems.push_back("hint_range on '" + u.name + "' has no argument list");
            return;
        }
        std::istringstream as(token.substr(open + 1, close - open - 1));
        std::string tok;
        std::vector<float> vals;
        while (std::getline(as, tok, ',')) {
            auto t = trim(tok);
            if (t.empty()) continue;
            auto value = parse_float(t);
            if (!value) {
                problems.push_back("hint_range on '" + u.name + "' has '" + t
                                   + "' where a number belongs");
                return;
            }
            vals.push_back(*value);
        }
        if (vals.size() >= 2) {
            u.range.min = vals[0];
            u.range.max = vals[1];
            u.range.declared = true;
        }
        if (vals.size() >= 3) {
            u.range.step = vals[2];
        }
        return;
    }

    if (token.rfind("hint_enum", 0) == 0) {
        // hint_enum("A,B,C") — the names of an int's values, in index order.
        // One quoted list rather than one string per name, so a name never
        // needs escaping and the declaration reads like the drop-down it is.
        const auto open = token.find('"');
        const auto close = token.rfind('"');
        if (open == std::string::npos || close == open) {
            problems.push_back("hint_enum on '" + u.name
                               + "' has no quoted list of names");
            return;
        }
        std::istringstream names(token.substr(open + 1, close - open - 1));
        std::string name;
        u.enum_options.clear();
        while (std::getline(names, name, ',')) {
            auto t = trim(name);
            if (t.empty()) {
                problems.push_back("hint_enum on '" + u.name + "' has an empty name");
                u.enum_options.clear();
                return;
            }
            u.enum_options.push_back(std::move(t));
        }
        if (u.hint != UniformHint::None && u.hint != UniformHint::Enum) {
            problems.push_back("uniform '" + u.name + "' declares more than one presentation "
                               "hint; 'hint_enum' wins");
        }
        u.hint = UniformHint::Enum;
        return;
    }

    if (token == "hint_emission") {
        u.emission = true;
        return;
    }
    if (token == "hint_albedo") {
        u.albedo = true;
        return;
    }

    auto hint = parse_hint_name(token);
    if (hint == UniformHint::None) {
        // A misspelt hint would otherwise silently change how the uniform
        // is presented and which fallback texture it gets.
        problems.push_back("unknown hint '" + token + "' on uniform '" + u.name + "'");
        return;
    }
    if (u.hint != UniformHint::None && u.hint != hint) {
        problems.push_back("uniform '" + u.name + "' declares more than one presentation "
                           "hint; '" + token + "' wins");
    }
    u.hint = hint;
}

// `bad_component` receives the text of the first component that was not a
// number, when one was not. Empty result with it unset means the declaration
// simply had no default.
static std::vector<float> parse_default_value(const std::string& val_str, UniformType type,
                                              std::string* bad_component = nullptr) {
    std::vector<float> result;
    // Match patterns like "0.5", "vec3(1.0, 0.5, 0.2)", "vec4(1.0)"
    std::string trimmed = val_str;

    // Strip vec2/vec3/vec4 wrapper
    static const std::regex vec_re(R"(vec[234]\s*\((.*)\))");
    std::smatch vm;
    if (std::regex_match(trimmed, vm, vec_re)) {
        trimmed = vm[1].str();
    }

    // Split by commas. A component that is not a number abandons the whole
    // default rather than contributing a zero: half a colour read from
    // `vec3(1.0, oops, 0.5)` is worse than falling back to the type's zero,
    // and `bad_component` lets the caller name what it choked on.
    std::istringstream ss(trimmed);
    std::string token;
    while (std::getline(ss, token, ',')) {
        auto t = trim(token);
        if (t.empty()) continue;
        auto value = parse_float(t);
        if (!value) {
            if (bad_component != nullptr) *bad_component = t;
            return {};
        }
        result.push_back(*value);
    }

    // If vec3(1.0) given, expand single value
    uint32_t expected = uniform_float_count(type);
    if (result.size() == 1 && expected > 1) {
        float v = result[0];
        result.resize(expected, v);
    }

    return result;
}

// A hook's body plus where it started, so a diagnostic can name the line the
// author wrote rather than a line in the generated file. The offset is the
// first character after the opening brace; `FjslBody::empty()` marks a hook
// the shader does not define.
//
// `code` is `source` with its comments masked (mask_comments): the hook is
// found and its braces matched there, so neither a comment naming the hook nor
// a brace in a comment can mislead it, and the body is cut from `source` with
// its comments intact.
static FjslBody extract_function_body(const std::string& source, const std::string& code,
                                      const std::string& func_name) {
    const size_t pos = find_function_declaration(code, func_name);
    if (pos == std::string::npos) return {};

    const size_t brace_start = code.find('{', pos);
    if (brace_start == std::string::npos) return {};

    int depth = 1;
    size_t i = brace_start + 1;
    while (i < code.size() && depth > 0) {
        if (code[i] == '{') depth++;
        else if (code[i] == '}') depth--;
        i++;
    }

    if (depth != 0) return {};
    return {source.substr(brace_start + 1, i - brace_start - 2), brace_start + 1};
}

// Remove the entire `<sig> { ... }` block for a given top-level function.
// Used to strip fragment/vertex/light from the source so what's left
// becomes user-authored file-scope code (helpers, constants).
//
// The blocks are located first and the string is rebuilt once. Erasing them
// one at a time shifted everything after each one down, which is quadratic in
// how many of them there are.
//
// The search runs on the source with its comments masked. A comment saying
// "before fragment() runs" used to be taken for the hook, and everything from
// that line to the next closing brace was erased: every helper after it went
// missing from the globals with no word said.
static void strip_function(std::string& source, const std::string& func_name) {
    const std::string code = mask_comments(source);
    // Match an identifier-boundary occurrence to avoid stripping
    // "fragment_helper" when looking for "fragment".
    std::vector<std::pair<size_t, size_t>> blocks;
    size_t search_from = 0;
    while (search_from < code.size()) {
        auto pos = code.find(func_name, search_from);
        if (pos == std::string::npos) break;
        // Check left boundary (start of source or non-identifier char before)
        bool left_ok = (pos == 0) || !is_identifier_char(code[pos - 1]);
        // Right side must be optional whitespace + '('
        size_t after = pos + func_name.size();
        size_t scan = after;
        while (scan < code.size() && std::isspace(static_cast<unsigned char>(code[scan]))) scan++;
        bool right_ok = (scan < code.size() && code[scan] == '(');
        if (!left_ok || !right_ok) {
            search_from = pos + 1;
            continue;
        }
        // Walk back to the start of the signature (newline before return type)
        size_t sig_start = pos;
        while (sig_start > 0 && code[sig_start - 1] != '\n') sig_start--;
        // Find '{'
        auto brace_start = code.find('{', scan);
        if (brace_start == std::string::npos) break;
        int depth = 1;
        size_t i = brace_start + 1;
        while (i < code.size() && depth > 0) {
            if (code[i] == '{') depth++;
            else if (code[i] == '}') depth--;
            i++;
        }
        if (depth != 0) break;
        // A signature can start before the previous block ended when the two
        // share a line; the earlier block wins, as it did when each erase
        // happened before the next search.
        if (!blocks.empty() && sig_start < blocks.back().second) {
            blocks.back().second = std::max(blocks.back().second, i);
        } else {
            blocks.emplace_back(sig_start, i);
        }
        search_from = i;
    }
    if (blocks.empty()) return;

    std::string out;
    out.reserve(source.size());
    size_t copied = 0;
    for (const auto& [begin, end] : blocks) {
        out.append(source, copied, begin - copied);
        copied = end;
    }
    out.append(source, copied, source.size() - copied);
    source.swap(out);
}

// Call `visit(offset, match)` for every match of `re` in `source`, in order.
//
// `re` must begin with the literal `keyword`, which is what the search runs
// on: std::regex costs tens of nanoseconds per byte wherever it is looking,
// whether or not it finds anything, so scanning a whole shader with one is
// most of what parsing it used to cost. Letting find() skip to the few places
// a match can begin, and anchoring the regex there, turns a scan of the file
// into a scan of the handful of declarations in it — the same matches, in the
// same order, for a fraction of the work.
//
// Offsets are relative to `source`; std::smatch positions are not, since the
// match runs from the keyword rather than from the start of the file.
template <typename Visit>
static void for_each_match(const std::string& source, std::string_view keyword,
                           const std::regex& re, Visit&& visit) {
    for (size_t at = source.find(keyword); at != std::string::npos;
         at = source.find(keyword, at + 1)) {
        std::smatch m;
        if (!std::regex_search(source.begin() + static_cast<std::ptrdiff_t>(at),
                               source.end(), m, re,
                               std::regex_constants::match_continuous)) {
            continue;
        }
        visit(at, m);
        // A match is consumed whole, the way regex_replace and
        // sregex_iterator advance past one rather than into it.
        at += static_cast<size_t>(m.length()) - 1;
    }
}

// Every match of `re` in `source`, as [begin, end) byte ranges.
static void collect_matches(const std::string& source, std::string_view keyword,
                            const std::regex& re,
                            std::vector<std::pair<size_t, size_t>>& out) {
    for_each_match(source, keyword, re, [&out](size_t at, const std::smatch& m) {
        out.emplace_back(at, at + static_cast<size_t>(m.length()));
    });
}

// Return user-written file-scope code: everything except shader_type,
// render_mode, uniform declarations, and the three reserved functions
// (fragment/vertex/light). Lets users author helper functions, constants,
// and structs that the spatial template injects at file scope.
//
// Each form is removed in its own round, and a round matches only what the
// rounds before it left standing. That order is observable on malformed
// input: an unterminated `uniform float x = 1.0` reaches for its semicolon on
// a later line, so what it swallows — and whether it is a uniform at all —
// depends on which lines are still there when its turn comes. The rounds stay
// because changing them would change what a broken shader reports.
//
// What is gone is their cost. A round now rewrites the string once, matching
// only at the few offsets where its keyword occurs, instead of handing the
// whole file to regex_replace to scan and copy from end to end.
static std::string extract_user_globals(const std::string& source, ShaderType type) {
    static const std::regex render_mode_re(R"(render_mode\s+[^;]+;)");

    auto cut_round = [](const std::string& text, std::string_view keyword,
                        const std::regex& re) {
        std::vector<std::pair<size_t, size_t>> found;
        collect_matches(text, keyword, re, found);
        if (found.empty()) return text;
        std::string out;
        out.reserve(text.size());
        size_t copied = 0;
        for (const auto& [begin, end] : found) {
            out.append(text, copied, begin - copied);
            copied = end;
        }
        out.append(text, copied, text.size() - copied);
        return out;
    };

    std::string s = cut_round(source, "shader_type", shader_type_re());
    s = cut_round(s, "render_mode", render_mode_re);
    // Headings are for the inspector; GLSL has never heard of them.
    s = cut_round(s, "group", group_declaration_re());
    // Strip uniform declarations.
    s = cut_round(s, "uniform", uniform_declaration_re());

    // Strip the entry points this kind of file has — their bodies are
    // extracted separately and substituted into the template. Whatever's left
    // is the user's file-scope code: helpers, structs, constants.
    //
    // Every hook the table knows is stripped, permitted or not: a file that
    // defines a refused one is already reported, and leaving the body in the
    // globals would follow that error with a second, confusing one from glslc.
    for (const auto& rule : hook_rules(type)) {
        strip_function(s, std::string(rule.hook));
    }
    return s;
}

namespace {
void log_diagnostics(const std::vector<FjslDiagnostic>& diagnostics,
                     const std::string& name) {
    const std::string file = name.empty() ? std::string{"<fjsl>"} : name;
    for (const auto& d : diagnostics) {
        if (d.severity == FjslSeverity::error) {
            FTK_GFX_ERROR("{}:{}:{}: error: {}", file, d.line, d.column, d.message);
        } else {
            FTK_GFX_WARN("{}:{}:{}: warning: {}", file, d.line, d.column, d.message);
        }
    }
}
} // namespace

void log_fjsl_diagnostics(const ShaderMetadata& meta, const std::string& name) {
    log_diagnostics(meta.diagnostics, name);
}

std::optional<ShaderType> declared_shader_type(const std::string& source) {
    std::smatch sm;
    if (!std::regex_search(source, sm, shader_type_re())) return std::nullopt;
    return shader_type_from_name(sm[1].str());
}

ShaderMetadata parse_fjsl(const std::string& source) {
    ShaderMetadata meta;

    // Record a problem against the text that caused it, so an editor can point
    // at it and a log line can read file:line:column. Parsing continues: one
    // bad declaration should not hide the rest of the file.
    PositionCursor cursor{source};
    auto diagnose = [&meta, &cursor](size_t offset, FjslSeverity severity,
                                     std::string message) {
        auto pos = cursor.at(offset);
        meta.diagnostics.push_back({pos.line, pos.column, severity, std::move(message)});
    };

    // Code is shared as plain GLSL, an include and its functions. A file from
    // when it was composed says where its code goes now, rather than only
    // that it lacks a shader_type.
    {
        std::smatch fn;
        if (std::regex_search(source, fn, shader_function_re())) {
            diagnose(static_cast<size_t>(fn.position(0)), FjslSeverity::error,
                     "shader_function is gone: put shared code in a .glsl file under "
                     "shaders/include and #include it");
            return meta;
        }
    }

    // Parse shader_type
    std::smatch sm;
    if (!std::regex_search(source, sm, shader_type_re())) {
        diagnose(0, FjslSeverity::error,
                 "missing shader_type declaration; expected 'shader_type spatial;'");
        return meta;
    }
    const std::string type_word = sm[1].str();
    if (auto type = shader_type_from_name(type_word)) {
        meta.shader_type = *type;
    } else {
        diagnose(static_cast<size_t>(sm.position(1)), FjslSeverity::error,
                 "unsupported shader_type '" + type_word
                     + "'; expected spatial, sky, ui, terrain or water");
        return meta;
    }

    // Parse render_mode (comma-separated keywords)
    static const std::regex render_mode_re(R"(render_mode\s+([^;]+);)");
    std::smatch rm;
    if (std::regex_search(source, rm, render_mode_re)) {
        std::istringstream modes(rm[1].str());
        std::string token;
        while (std::getline(modes, token, ',')) {
            // Trim whitespace
            auto s = token.find_first_not_of(" \t");
            auto e = token.find_last_not_of(" \t");
            if (s == std::string::npos) continue;
            std::string mode = token.substr(s, e - s + 1);

            if (mode == "unlit") meta.render_mode = RenderMode::Unlit;
            else if (mode == "no_weather") meta.no_weather = true;
            else if (mode == "cull_disabled") meta.render_state.cull_disabled = true;
            else if (mode == "cull_front") meta.render_state.cull_front = true;
            else if (mode == "depth_write_disabled") meta.render_state.depth_write_disabled = true;
            else if (mode == "depth_test_disabled") meta.render_state.depth_test_disabled = true;
            else if (mode == "blend_alpha") { meta.render_state.blend = BlendMode::Alpha; meta.render_state.force_transparent = true; }
            else if (mode == "blend_add") { meta.render_state.blend = BlendMode::Add; meta.render_state.force_transparent = true; }
            else if (mode == "blend_multiply") { meta.render_state.blend = BlendMode::Multiply; meta.render_state.force_transparent = true; }
            else {
                diagnose(static_cast<size_t>(rm.position(1)), FjslSeverity::warning,
                         "unknown render_mode '" + mode + "'");
            }
        }
    }

    uint32_t texture_slot = PBR_TEXTURE_SLOTS; // custom textures follow the PBR set
    uint32_t param_offset = 0;

    // Every heading and where it was declared, so each uniform can be given
    // the last one above it. Walking the two in step beats searching backwards
    // from each uniform.
    std::vector<std::pair<std::size_t, std::string>> groups;
    for_each_match(source, "group", group_declaration_re(),
                   [&](size_t at, const std::smatch& g) {
                       groups.emplace_back(at, g[1].str());
                   });
    std::size_t next_group = 0;
    std::string current_group;

    for_each_match(source, "uniform", uniform_declaration_re(),
                   [&](size_t at, const std::smatch& m) {
        while (next_group < groups.size() && groups[next_group].first < at) {
            current_group = groups[next_group].second;
            ++next_group;
        }

        FjslUniform u;
        u.group = current_group;
        u.type = parse_type(m[1].str());
        u.name = m[2].str();

        if (m[3].matched) {
            std::vector<std::string> hint_problems;
            for (const auto& token : split_hint_list(m[3].str())) {
                apply_hint(u, token, hint_problems);
            }
            for (auto& problem : hint_problems) {
                diagnose(at + static_cast<size_t>(m.position(3)), FjslSeverity::warning,
                         std::move(problem));
            }
            if (u.hint == UniformHint::Lut && u.type != UniformType::Sampler2D) {
                diagnose(at + static_cast<size_t>(m.position(3)), FjslSeverity::warning,
                         "hint_lut on '" + u.name + "' names a table to sample; a "
                             + std::string(glsl_type_name(u.type)) + " ignores it");
                u.hint = UniformHint::None;
            }
            if (u.hint == UniformHint::Enum && u.type != UniformType::Int) {
                diagnose(at + static_cast<size_t>(m.position(3)), FjslSeverity::warning,
                         "hint_enum on '" + u.name + "' names the values of an int; a "
                             + std::string(glsl_type_name(u.type)) + " ignores it");
                u.hint = UniformHint::None;
                u.enum_options.clear();
            }
        }

        if (m[4].matched) {
            std::string def_str = m[4].str();
            // Trim whitespace
            auto s = def_str.find_first_not_of(" \t");
            auto e = def_str.find_last_not_of(" \t");
            if (s != std::string::npos) {
                def_str = def_str.substr(s, e - s + 1);
            }
            std::string bad_component;
            u.default_value = parse_default_value(def_str, u.type, &bad_component);
            if (!bad_component.empty()) {
                diagnose(at + static_cast<size_t>(m.position(4)), FjslSeverity::warning,
                         "uniform '" + u.name + "' has '" + bad_component
                             + "' where its default value belongs; using zero");
            }
        }

        if (u.type == UniformType::Sampler2D) {
            if (texture_slot >= TOTAL_TEXTURE_SLOTS) {
                // Skipping leaves the name undefined in the generated GLSL, so
                // the real complaint arrives later as an undeclared identifier
                // somewhere the author cannot place. Say it here, where the
                // declaration is.
                diagnose(at + static_cast<size_t>(m.position(2)), FjslSeverity::error,
                         "uniform '" + u.name + "' is the "
                             + std::to_string(texture_slot - PBR_TEXTURE_SLOTS + 1)
                             + "th custom sampler2D; a material has room for "
                             + std::to_string(CUSTOM_TEXTURE_SLOTS));
                return;
            }
            u.slot = texture_slot++;
            meta.custom_texture_count++;
        } else {
            u.slot = param_offset;
            uint32_t count = uniform_float_count(u.type);
            param_offset += count;
        }

        meta.uniforms.push_back(u);
    });

    meta.custom_param_count = param_offset;

    // A composition, from before shared code was plain GLSL.
    for_each_match(source, "use", use_head_re(), [&](size_t offset, const std::smatch& m) {
        diagnose(offset, FjslSeverity::error,
                 "use '" + m[1].str() + "' is gone: put the code of '" + m[2].str()
                     + "' in a .glsl file under shaders/include, #include it and call "
                       "its functions");
    });

    // Sky, fog and post shaders supply their user code via sky(), density()
    // and color() rather than fragment(); all route through the same fragment_body slot
    // so the /*FJSL_USER_CODE*/ substitution path serves every type.
    // Hooks are looked for where there is code, never in a comment.
    const std::string code = mask_comments(source);
    const char* body_hook = "fragment";
    if (meta.shader_type == ShaderType::Sky) body_hook = "sky";
    if (meta.shader_type == ShaderType::Fog) body_hook = "density";
    if (meta.shader_type == ShaderType::Post) body_hook = "color";
    meta.fragment_body = extract_function_body(source, code, body_hook);
    meta.vertex_body = extract_function_body(source, code, "vertex");
    meta.light_body = extract_function_body(source, code, "light");
    meta.layers_body = extract_function_body(source, code, "layers");
    meta.user_globals = extract_user_globals(source, meta.shader_type);
    // Kept so the generator can turn a body's byte offset into a line number.
    meta.source_text = source;

    // A hook that a shader type cannot run is an error rather than silently
    // ignored code. A ui shader paints one screen-aligned quad: there is no
    // geometry to displace and no light to shade. Terrain geometry is
    // generated in a mesh shader from the height texture, and water's from
    // its wave field, so neither has anything for vertex() to move — a water
    // surface is shaped by its .fjwater, not by its material. layers()
    // decides how the painted layer weights blend and only terrain has
    // layers.
    // A hook this kind of file cannot run is an error rather than silently
    // ignored code — the same table that decides which bodies get stripped
    // from the globals, read for the other half of its job. The chain this
    // replaced had grown unevenly: sky was guarded against nothing, so a sky
    // shader defining fragment() instead of sky() compiled to a black sky with
    // no word said.
    const std::string kind = "shader_type " + std::string(shader_type_name(meta.shader_type));
    for (const auto& rule : hook_rules(meta.shader_type)) {
        if (rule.refusal.empty()) continue;

        const size_t at = find_function_declaration(code, rule.hook);
        if (at == std::string::npos) continue;

        diagnose(at, FjslSeverity::error,
                 kind + " has no " + std::string(rule.hook) + "() stage — "
                     + std::string(rule.refusal));
    }

    // Where the code names a uniform is decided by GLSL's scoping (see
    // scan_uniform_names). Two things GLSL would refuse are refused here,
    // where they can be pointed at: writing a uniform, which holds the
    // material's value and nothing else, and declaring its name again at file
    // scope, which would leave every read of it meaning something else.
    {
        UniformNames names;
        for (const auto& u : meta.uniforms) names.insert(u.name);
        const UniformScan scan = scan_uniform_names(code, names, false);
        for (const auto& write : scan.writes) {
            diagnose(write.offset, FjslSeverity::error,
                     "'" + write.name + "' is a uniform, which a shader reads but cannot "
                     "write: copy it into a local to change it");
        }
        for (const auto& again : scan.redeclared) {
            diagnose(again.offset, FjslSeverity::error,
                     "'" + again.name + "' is already a uniform: nothing at file scope "
                     "can take its name");
        }
    }

    // An empty fragment() is standard PBR and an empty sky() is black, but a
    // fog shader with no density() makes no fog at all, which reads as the
    // pass being broken rather than the file being incomplete.
    if (meta.shader_type == ShaderType::Fog
        && find_function_declaration(code, "density") == std::string::npos) {
        diagnose(0, FjslSeverity::warning,
                 "a shader_type fog shader with no density() makes no fog");
    }
    // An empty color() is not the engine's look: it shows the exposed scene
    // clamped, with no tone curve, which reads as a broken frame.
    if (meta.shader_type == ShaderType::Post
        && find_function_declaration(code, "color") == std::string::npos) {
        diagnose(0, FjslSeverity::warning,
                 "a shader_type post shader with no color() shows the scene with no tone curve");
    }

    return meta;
}

namespace {

// The 1-based line a byte offset falls on.
uint32_t line_at(const std::string& source, size_t offset) {
    return 1 + static_cast<uint32_t>(
               std::count(source.begin(),
                          source.begin() + static_cast<std::ptrdiff_t>(
                                               std::min(offset, source.size())),
                          '\n'));
}

// Wrap a hook's body so a compiler error inside it names the .fjsl and the
// line the author wrote, instead of a line in the generated file.
//
// `#line N` names the line AFTER the directive, so the body's own first line
// is N. The trailing directive hands numbering back to the template; its line
// is filled in by the caller, which is the only thing that knows where in the
// template the body landed.
//
// A shader with no source_path still gets the numeric form, which fixes the
// line number even though errors keep naming the generated file.
std::string with_line_markers(const FjslBody& body, const ShaderMetadata& meta) {
    if (body.empty()) return body.code;

    std::string out = "\n#line " + std::to_string(line_at(meta.source_text, body.offset));
    if (!meta.source_path.empty()) {
        out += " \"" + meta.source_path + "\"";
    }
    out += "\n" + body.code + "\n";
    return out;
}

// What the generated file calls itself once a body has handed numbering back.
// Not a real path — the generated file's own name varies by stage and the
// templates do not know it — but it has to be something other than the .fjsl,
// or template code inherits the author's filename.
constexpr const char* GENERATED_NAME = "<generated>";

// Filename-based #line is an extension. glslc enables
// GL_GOOGLE_cpp_style_line_directive by default, so this is belt and braces —
// it costs one line and keeps the generated GLSL correct if it is ever fed to
// libshaderc or glslang directly, where the default is disabled.
//
// #extension has to follow #version, so this splices after the first line
// rather than prepending.
void enable_line_directives(std::string& glsl) {
    auto eol = glsl.find('\n');
    if (eol == std::string::npos) return;
    glsl.insert(eol + 1, "#extension GL_GOOGLE_cpp_style_line_directive : require\n");
}

// A function a shader file defines at file scope: the stretch of its globals
// it covers, comment and signature included, and its name.
struct GlobalFunction {
    size_t begin{0};
    size_t end{0};
    std::string name;
};

// The functions `globals` defines, in order. Each covers what leads up to it
// since the declaration before, so a function's comment goes with it. A
// definition is told from a struct or an initializer list by what its opening
// brace follows: the `)` closing its parameters.
std::vector<GlobalFunction> global_functions(std::string_view text) {
    std::vector<GlobalFunction> out;
    size_t item_start = 0;
    int parens = 0;
    std::string name;  // the identifier before the item's first '('
    char last = 0;     // the last character that is not space or comment
    bool line_start = true;
    size_t i = 0;
    while (i < text.size()) {
        if (const size_t c = comment_length(text, i)) {
            i += c;
            continue;
        }
        const char ch = text[i];
        if (ch == '\n') {
            line_start = true;
            ++i;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(ch))) {
            ++i;
            continue;
        }
        if (line_start && ch == '#') {
            // A preprocessor line, carried on by a trailing backslash, is a
            // declaration of its own.
            size_t end = text.find('\n', i);
            while (end != std::string_view::npos && end > 0 && text[end - 1] == '\\') {
                end = text.find('\n', end + 1);
            }
            i = end == std::string_view::npos ? text.size() : end;
            item_start = i;
            name.clear();
            last = 0;
            continue;
        }
        line_start = false;
        if (ch == '(') {
            if (parens == 0 && name.empty()) {
                size_t e = i;
                while (e > item_start && std::isspace(static_cast<unsigned char>(text[e - 1]))) --e;
                size_t b = e;
                while (b > item_start && is_identifier_char(text[b - 1])) --b;
                name = std::string(text.substr(b, e - b));
            }
            ++parens;
        } else if (ch == ')') {
            if (parens > 0) --parens;
        } else if (ch == '{' && parens == 0) {
            int depth = 1;
            size_t j = i + 1;
            while (j < text.size() && depth > 0) {
                if (const size_t c = comment_length(text, j)) {
                    j += c;
                    continue;
                }
                if (text[j] == '{') ++depth;
                else if (text[j] == '}') --depth;
                ++j;
            }
            if (last == ')' && !name.empty()) {
                out.push_back({item_start, j, name});
                item_start = j;
                name.clear();
                last = 0;
            } else {
                // A struct or an initializer list: the declaration ends at its ';'.
                last = '}';
            }
            i = j;
            continue;
        } else if (ch == ';' && parens == 0) {
            item_start = i + 1;
            name.clear();
            last = 0;
            ++i;
            continue;
        }
        last = ch;
        ++i;
    }
    return out;
}

// `globals` keeping only the functions one stage reaches: those its hooks
// call, and those they call in turn. Everything that is not a function stays,
// constants, structs, #defines and prototypes, and what it calls is reached
// too, a macro wrapping a helper included.
//
// glslang type-checks every function it is given, called or not, so a helper
// reading a fragment built-in fails a vertex stage that never calls it, and a
// helper moving VERTEX fails the fragment stage. Godot's shading language
// treats a function's stage the same way.
std::string globals_reached_from(const std::string& globals,
                                 const std::vector<std::string_view>& roots) {
    const auto functions = global_functions(globals);
    if (functions.empty()) return globals;

    std::map<std::string, std::vector<size_t>, std::less<>> by_name;
    for (size_t f = 0; f < functions.size(); ++f) by_name[functions[f].name].push_back(f);

    std::vector<bool> reached(functions.size(), false);
    std::vector<size_t> work;
    // Every whole word in `text` followed by '(' that names a function.
    auto visit = [&](std::string_view text) {
        size_t at = 0;
        while (at < text.size()) {
            if (const size_t c = comment_length(text, at)) {
                at += c;
                continue;
            }
            if (!is_identifier_char(text[at])) {
                ++at;
                continue;
            }
            const size_t start = at;
            while (at < text.size() && is_identifier_char(text[at])) ++at;
            size_t after = at;
            while (after < text.size() && std::isspace(static_cast<unsigned char>(text[after]))) ++after;
            if (after >= text.size() || text[after] != '(') continue;
            const auto found = by_name.find(text.substr(start, at - start));
            if (found == by_name.end()) continue;
            for (size_t f : found->second) {
                if (!reached[f]) {
                    reached[f] = true;
                    work.push_back(f);
                }
            }
        }
    };

    for (std::string_view root : roots) visit(root);
    const std::string_view all = globals;
    size_t outside = 0;
    for (const auto& f : functions) {
        visit(all.substr(outside, f.begin - outside));
        outside = f.end;
    }
    visit(all.substr(outside));
    while (!work.empty()) {
        const size_t f = work.back();
        work.pop_back();
        visit(all.substr(functions[f].begin, functions[f].end - functions[f].begin));
    }

    std::string out;
    out.reserve(globals.size());
    size_t copied = 0;
    for (size_t f = 0; f < functions.size(); ++f) {
        if (reached[f]) continue;
        out.append(globals, copied, functions[f].begin - copied);
        copied = functions[f].end;
    }
    out.append(globals, copied, std::string::npos);
    return out;
}

} // namespace

namespace {

/// How generated GLSL reaches a program's uniforms: `param(i)` is the
/// expression reading float `i` of its parameter block, `texture(slot)` the
/// sampler in texture slot `slot`, numbered as FjslUniform::slot numbers
/// samplers: from PBR_TEXTURE_SLOTS, after a material's own four. Each shader
/// type's template fixes these.
struct FjslUniformAccess {
    std::function<std::string(uint32_t index)> param;
    std::function<std::string(uint32_t slot)> texture;
};

/// The GLSL expression reading a scalar uniform whose first float is at
/// `first` of the parameter block: a float as it is, an int rounded to the
/// whole number a track or a blend may have left between two, a vector from
/// its components.
std::string uniform_read(const FjslUniform& u, const FjslUniformAccess& access, uint32_t first) {
    if (u.type == UniformType::Int) return "int(round(" + access.param(first) + "))";
    const uint32_t count = uniform_float_count(u.type);
    if (count == 1) return access.param(first);
    std::string out = std::string(glsl_type_name(u.type)) + "(";
    for (uint32_t i = 0; i < count; ++i) {
        if (i > 0) out += ", ";
        out += access.param(first + i);
    }
    return out + ")";
}

/// The functions that read the shader's uniforms, one each, named by
/// uniform_accessor(). The shader's own code calls them wherever a name means
/// the uniform (resolve_uniform_reads), so a uniform is read where it is used,
/// as cheap as reading the buffer there directly. `samplers` is false for a
/// stage with no texture access, which leaves them out.
std::string emit_uniform_accessors(const ShaderMetadata& meta, const FjslUniformAccess& access,
                                   bool samplers) {
    std::string out;
    for (const auto& u : meta.uniforms) {
        const bool sampler = u.type == UniformType::Sampler2D;
        if (sampler && !samplers) continue;
        out += std::string(sampler ? "FjellTexture" : glsl_type_name(u.type)) + " "
             + uniform_accessor(u.name) + "() { return "
             + (sampler ? access.texture(u.slot) : uniform_read(u, access, u.slot)) + "; }\n";
    }
    return out;
}

/// The names of the uniforms a stage reads, for resolve_uniform_reads.
UniformNames uniform_names(const ShaderMetadata& meta, bool samplers) {
    UniformNames names;
    for (const auto& u : meta.uniforms) {
        if (u.type == UniformType::Sampler2D && !samplers) continue;
        names.insert(u.name);
    }
    return names;
}

} // namespace

std::string generate_fragment_shader(const ShaderMetadata& meta,
                                     const std::string& template_source) {
    std::string result = template_source;
    const std::string generated_name = GENERATED_NAME;

    // Each shader type reaches its material row differently. Sky is a single
    // fullscreen pass with no draws[] SSBO, so the row index is pushed as
    // `push.sky_material_id`; terrain and water likewise push their row per
    // node, and the fog compute pass pushes the fog node's; a ui quad carries
    // its row in the quad record; spatial shaders look up the material via
    // per-draw data.
    const std::string mat_expr = [&]() -> std::string {
        switch (meta.shader_type) {
            case ShaderType::Sky:     return "materials[push.sky_material_id]";
            case ShaderType::Terrain: return "materials[push.terrain_material_id]";
            case ShaderType::Water:   return "materials[push.water_material_id]";
            case ShaderType::Fog:     return "materials[pc.fog_material_id]";
            case ShaderType::Ui:      return "materials[quads[frag_quad_index].material_id]";
            case ShaderType::Post:    return "materials[push.material_id]";
            case ShaderType::Spatial: break;
        }
        return "materials[draws[frag_draw_index].material_id]";
    }();
    // Scalars read the material row's slice of the params SSBO, except for
    // post, whose values differ per viewport and come from the post pass's
    // own buffer (see fjell_post_template.glsl). Samplers are handles of the
    // bindless table made from the row's texture indices.
    const bool is_post = meta.shader_type == ShaderType::Post;
    const std::string param_array = is_post ? "post_params" : "custom_params";
    const std::string param_base = is_post ? "push.param_base" : mat_expr + ".param_offset";
    const FjslUniformAccess access{
        .param = [&](uint32_t index) {
            return param_array + "[" + param_base + " + " + std::to_string(index) + "]";
        },
        .texture = [&](uint32_t slot) {
            return "FjellTexture(" + mat_expr + ".texture_indices[" + std::to_string(slot) + "])";
        },
    };
    const UniformNames uniforms = uniform_names(meta, true);

    // Substitute placeholders
    auto replace = [&](const std::string& placeholder, const std::string& replacement) {
        auto pos = result.find(placeholder);
        if (pos != std::string::npos) {
            result.replace(pos, placeholder.size(), replacement);
        }
    };

    // Substitute a user-authored body, bracketed by #line so a compiler error
    // inside it names the .fjsl. The closing directive restores the template's
    // own numbering: it can only be computed here, once the body's position in
    // the generated file is known.
    auto replace_body = [&](const std::string& placeholder, const FjslBody& body) {
        auto pos = result.find(placeholder);
        if (pos == std::string::npos || body.empty()) {
            replace(placeholder, body.code);
            return;
        }
        // The closing directive must restate the generated file's name, not
        // just its line: a numeric-only `#line` leaves the filename set to
        // whatever the last one named, so every error after a body would go on
        // blaming the .fjsl for template code the author never wrote.
        const uint32_t resume = line_at(result, pos) + 1;
        FjslBody resolved = body;
        resolved.code = resolve_uniform_reads(body.code, uniforms, true);
        std::string text = with_line_markers(resolved, meta)
                         + "#line " + std::to_string(resume)
                         + " \"" + generated_name + "\"\n";
        result.replace(pos, placeholder.size(), text);
    };

    // The file's own helpers that this stage's hooks reach, at file scope and
    // above main(): a template without an FJSL_LIGHTING_END marker has its
    // unlit path cut to main()'s closing brace, found with rfind('}'), so
    // nothing may be emitted after it. A helper written for vertex() stays
    // out of this stage.
    const std::vector<std::string_view> roots{meta.fragment_body.code, meta.light_body.code,
                                              meta.layers_body.code};
    const std::string globals =
        resolve_uniform_reads(globals_reached_from(meta.user_globals, roots), uniforms, false);
    // A sampler uniform is a texture of the bindless table, which travels as
    // a handle (fjell_bindless.glsl): GLSL never passes a texture and sampler
    // combined. A function the author writes taking a sampler2D takes the
    // handle, by a name mapped around the author's code alone, so what they
    // wrote stays as written.
    replace("/*FJSL_USER_GLOBALS*/", emit_uniform_accessors(meta, access, true)
                                     + "#define sampler2D FjellTexture\n" + globals
                                     + "#undef sampler2D\n");
    replace_body("/*FJSL_USER_CODE*/", meta.fragment_body);
    replace_body("/*FJSL_LAYERS_USER_CODE*/", meta.layers_body);

    // The weather's default response to the surface: rain wets it, snow
    // covers it. Off for unlit shaders, which have no surface to speak of,
    // and for shaders that answer the weather themselves.
    replace("/*FJSL_WEATHER*/",
            (meta.render_mode == RenderMode::Unlit || meta.no_weather) ? std::string{} :
            std::string("    fjell_apply_weather(ALBEDO, ROUGHNESS, METALLIC, NORMAL, normalize(frag_world_normal), WORLD_POS,\n"
                        "                        WETNESS, SNOW, PRECIPITATION, SKY_EXPOSURE, TIME);\n"));

    // light() body — user-provided replaces the default PBR. The default
    // mirrors the standard GGX math so shaders without a custom light()
    // render identically to the pre-hook template.
    static const char* DEFAULT_LIGHT_BODY = R"GLSL(
    vec3 H = normalize(VIEW_DIR + LIGHT_DIR);
    float NdotV = max(dot(NORMAL, VIEW_DIR), 0.001);
    float NdotH = max(dot(NORMAL, H), 0.0);
    float HdotV = max(dot(H, VIEW_DIR), 0.0);
    float D = distribution_GGX(NdotH, ROUGHNESS);
    float G = geometry_smith(NdotV, NdotL, ROUGHNESS);
    vec3  F = fresnel_schlick(HdotV, F0);
    vec3 spec = (D * G * F) / (4.0 * NdotV * NdotL + 0.0001);
    vec3 kD = (vec3(1.0) - F) * (1.0 - METALLIC);
    DIFFUSE_LIGHT  += kD * ALBEDO / PI;
    SPECULAR_LIGHT += spec;
)GLSL";
    // Fog's light() is per light per froxel and scatters by a phase angle
    // rather than shading by a normal. The default is the Henyey-Greenstein
    // lobe the fog pass always used, clamped so the forward peak stays gentle
    // and the sun does not smear a bright blob through the medium; ambient
    // arrives from everywhere at once and scatters by the isotropic average.
    static const char* DEFAULT_FOG_LIGHT_BODY = R"GLSL(
    float phase = LIGHT_KIND == FOG_LIGHT_AMBIENT
        ? ISOTROPIC_PHASE
        : min(henyey_greenstein(0.7, COS_THETA), 0.1);
    SCATTERED += LIGHT_COLOR * phase * SHADOW;
)GLSL";
    // The default body is engine code, so it gets no #line — an error in it is
    // an engine bug and should point at the generated file.
    if (meta.light_body.empty()) {
        replace("/*FJSL_LIGHT_USER_CODE*/",
                std::string(meta.shader_type == ShaderType::Fog ? DEFAULT_FOG_LIGHT_BODY
                                                                : DEFAULT_LIGHT_BODY));
    } else {
        replace_body("/*FJSL_LIGHT_USER_CODE*/", meta.light_body);
    }

    // Replace PBR lighting block with direct output for unlit shaders
    if (meta.render_mode == RenderMode::Unlit) {
        const std::string lighting_marker = "    // \xe2\x94\x80\xe2\x94\x80 PBR lighting";
        auto lighting_pos = result.find(lighting_marker);
        if (lighting_pos != std::string::npos) {
            // The block ends where the template says it does, so that what a
            // template puts after its lighting — a debug view, say — is kept
            // for an unlit shader too. A template that does not say has its
            // block run to the closing brace of main(), the last '}' in the
            // file, and may put nothing after the lighting call.
            const std::string end_marker = "/*FJSL_LIGHTING_END*/";
            auto main_end = result.find(end_marker, lighting_pos);
            if (main_end != std::string::npos) {
                main_end += end_marker.size();
            } else {
                main_end = result.rfind('}');
            }
            if (main_end != std::string::npos) {
                // Every attachment the pass binds has to be written, including
                // the ones an unlit surface has no opinion about: a fragment
                // that leaves one alone gets undefined values in it, and the
                // velocity buffer feeding undefined values to TAA is a smear
                // that only shows in motion.
                //
                // The normal goes out zero, the same as geometry drawn without
                // one, and the velocity is the real screen-space motion, which
                // the clip positions give without any lighting. Both are in
                // scope in every template this substitution can reach: sky and
                // ui have no PBR lighting block to replace.
                result.replace(lighting_pos, main_end - lighting_pos,
                    "    out_color = vec4(ALBEDO + EMISSION, ALPHA);\n"
                    "    out_metallic_roughness = vec2(0.0);\n"
                    "    out_normal = vec2(0.0);\n"
                    "    {\n"
                    "        vec2 unlit_cur = frag_clip_pos.xy / frag_clip_pos.w;\n"
                    "        vec2 unlit_prev = frag_prev_clip_pos.xy / frag_prev_clip_pos.w;\n"
                    "        out_velocity = (unlit_cur - unlit_prev) * 0.5;\n"
                    "    }\n");
            }
        }
    }

    enable_line_directives(result);
    return result;
}

std::string generate_vertex_shader(const ShaderMetadata& meta,
                                    const std::string& template_source) {
    std::string result = template_source;

    // The uniforms read as on the fragment side, so a helper reads them just
    // as vertex() does. The vertex stage has no texture access, so samplers
    // are left out. `mat` is the template's, set before the hook runs.
    const FjslUniformAccess access{
        .param = [](uint32_t index) { return "custom_params[mat.param_offset + " + std::to_string(index) + "]"; },
        .texture = {},
    };
    const UniformNames uniforms = uniform_names(meta, false);
    std::string globals = emit_uniform_accessors(meta, access, false);
    // The file's own helpers that vertex() reaches; one written for
    // fragment() stays out, since it may read what this stage lacks.
    globals += resolve_uniform_reads(globals_reached_from(meta.user_globals, {meta.vertex_body.code}),
                                     uniforms, false);
    FjslBody body = meta.vertex_body;
    body.code = resolve_uniform_reads(body.code, uniforms, true);

    auto replace = [&](const std::string& placeholder, const std::string& replacement) {
        auto pos = result.find(placeholder);
        if (pos != std::string::npos) {
            result.replace(pos, placeholder.size(), replacement);
        }
    };

    replace("/*FJSL_VERTEX_GLOBALS*/", globals);

    // The same body is substituted into all six vertex/mesh templates, so a
    // #line here means one authored line maps to the right place in every one
    // of them rather than to six different generated lines.
    {
        auto pos = result.find("/*FJSL_VERTEX_USER_CODE*/");
        if (pos != std::string::npos && !body.empty()) {
            const uint32_t resume = line_at(result, pos) + 1;
            result.replace(pos, std::string("/*FJSL_VERTEX_USER_CODE*/").size(),
                           with_line_markers(body, meta)
                               + "#line " + std::to_string(resume)
                               + " \"" + GENERATED_NAME + "\"\n");
        } else {
            replace("/*FJSL_VERTEX_USER_CODE*/", body.code);
        }
    }

    enable_line_directives(result);
    return result;
}

} // namespace fjell
