#include "renderer/resources/fjsl_parser.hpp"
#include "core/log.hpp"
#include "core/math/color_space.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <regex>
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
    {"water", ShaderType::Water},
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

// A shader_function: one entry point, none of a Type's hooks.
constexpr HookRule FUNCTION_HOOKS[] = {
    {"main", {}},
    {"fragment", "a shader_function has one entry point, main()"},
    {"vertex", "a shader_function has one entry point, main()"},
    {"light", "a shader_function has one entry point, main()"},
    {"sky", "a shader_function has one entry point, main()"},
    {"layers", "a shader_function has one entry point, main()"},
};

constexpr HookRule SPATIAL_HOOKS[] = {
    {"fragment", {}}, {"vertex", {}}, {"light", {}},
    {"sky", "only a shader_type sky shader writes SKY"},
    {"layers", "only terrain blends painted layers"},
};
constexpr HookRule SKY_HOOKS[] = {
    {"sky", {}},
    {"fragment", "a sky shader writes SKY from sky(), not ALBEDO from fragment()"},
    {"vertex", "the sky is drawn as one fullscreen triangle"},
    {"light", "the sky is not lit; it is the light"},
    {"layers", "only terrain blends painted layers"},
};
constexpr HookRule UI_HOOKS[] = {
    {"fragment", {}},
    {"vertex", "a ui quad has no geometry to displace"},
    {"light", "a ui quad is not lit"},
    {"sky", "only a shader_type sky shader writes SKY"},
    {"layers", "only terrain blends painted layers"},
};
constexpr HookRule TERRAIN_HOOKS[] = {
    {"fragment", {}}, {"light", {}}, {"layers", {}},
    {"vertex", "terrain geometry is generated in a mesh shader from the height texture"},
    {"sky", "only a shader_type sky shader writes SKY"},
};
constexpr HookRule WATER_HOOKS[] = {
    {"fragment", {}}, {"light", {}},
    {"vertex", "the surface comes from the .fjwater wave field"},
    {"sky", "only a shader_type sky shader writes SKY"},
    {"layers", "only terrain blends painted layers"},
};

std::span<const HookRule> hook_rules(std::optional<ShaderType> type) {
    if (!type) return FUNCTION_HOOKS;
    switch (*type) {
        case ShaderType::Sky:     return SKY_HOOKS;
        case ShaderType::Ui:      return UI_HOOKS;
        case ShaderType::Terrain: return TERRAIN_HOOKS;
        case ShaderType::Water:   return WATER_HOOKS;
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
// diagnostic, which is quadratic in a file that produces many of them — and a
// composed shader inherits a diagnostic from every contribution. The cursor
// remembers the last offset it reached so a parse that reports in increasing
// order walks the source once in total; an offset behind the cursor simply
// restarts, which keeps the answer right whatever order the caller asks in.
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
        values[i] = color_space::srgb_to_linear(values[i]);
    }
}

void decode_uniform_from_gpu(const FjslUniform& u, std::span<float> values) {
    if (!is_authored_color(u)) return;
    const size_t rgb = std::min<size_t>(values.size(), 3);
    for (size_t i = 0; i < rgb; ++i) {
        values[i] = color_space::linear_to_srgb(values[i]);
    }
}

static UniformType parse_type(const std::string& t) {
    if (t == "float") return UniformType::Float;
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

/// `shader_function;` — this file is composed into others, drawn by nothing.
static const std::regex& shader_function_re() {
    static const std::regex re(R"(shader_function\s*;)");
    return re;
}

/// `in <type> <name>;` / `out <type> <name>;` — a shader_function's interface.
/// Groups: 1 type, 2 name. No defaults: the consumer passes everything, so a
/// function never reaches for a built-in of whatever happens to call it.
static const std::regex& in_port_re() {
    static const std::regex re(R"(in\s+(\w+)\s+(\w+)\s*;)");
    return re;
}
static const std::regex& out_port_re() {
    static const std::regex re(R"(out\s+(\w+)\s+(\w+)\s*;)");
    return re;
}

/// `use <name> = "<path>"(` — the head of a wiring. The argument list is not
/// matched here: an argument is a consumer-side GLSL expression and may contain
/// its own parentheses, which no regex of the `[^)]*` shape survives — it stops
/// at the first `)` and the whole declaration silently fails to match. The
/// caller scans from the `(` counting depth instead.
/// Groups: 1 name, 2 path.
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
        }
        if (vals.size() >= 3) {
            u.range.step = vals[2];
        }
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
// Brace matching is not string- or comment-aware: a `}` inside a comment
// truncates the body. No shader in the engine or the test project has one, so
// this is latent rather than live, but it is why a body that looks right can
// come back short.
static FjslBody extract_function_body(const std::string& source, const std::string& func_name) {
    auto pos = source.find("void " + func_name);
    if (pos == std::string::npos) return {};

    // Find opening brace
    auto brace_start = source.find('{', pos);
    if (brace_start == std::string::npos) return {};

    // Match braces
    int depth = 1;
    size_t i = brace_start + 1;
    while (i < source.size() && depth > 0) {
        if (source[i] == '{') depth++;
        else if (source[i] == '}') depth--;
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
// how many of them there are — invisible in a hand-written shader with a
// single fragment(), but composition inlines a hook per contribution.
static void strip_function(std::string& source, const std::string& func_name) {
    // Match an identifier-boundary occurrence to avoid stripping
    // "fragment_helper" when looking for "fragment".
    std::vector<std::pair<size_t, size_t>> blocks;
    size_t search_from = 0;
    while (search_from < source.size()) {
        auto pos = source.find(func_name, search_from);
        if (pos == std::string::npos) break;
        // Check left boundary (start of source or non-identifier char before)
        bool left_ok = (pos == 0) || !is_identifier_char(source[pos - 1]);
        // Right side must be optional whitespace + '('
        size_t after = pos + func_name.size();
        size_t scan = after;
        while (scan < source.size() && std::isspace(static_cast<unsigned char>(source[scan]))) scan++;
        bool right_ok = (scan < source.size() && source[scan] == '(');
        if (!left_ok || !right_ok) {
            search_from = pos + 1;
            continue;
        }
        // Walk back to the start of the signature (newline before return type)
        size_t sig_start = pos;
        while (sig_start > 0 && source[sig_start - 1] != '\n') sig_start--;
        // Find '{'
        auto brace_start = source.find('{', scan);
        if (brace_start == std::string::npos) break;
        int depth = 1;
        size_t i = brace_start + 1;
        while (i < source.size() && depth > 0) {
            if (source[i] == '{') depth++;
            else if (source[i] == '}') depth--;
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
static std::string extract_user_globals(const std::string& source,
                                        std::optional<ShaderType> type) {
    static const std::regex shader_type_re(R"(shader_type\s+\w+\s*;)");
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

    std::string s = cut_round(source, "shader_type", shader_type_re);
    s = cut_round(s, "shader_function", shader_function_re());
    s = cut_round(s, "render_mode", render_mode_re);
    // A function's interface: parsed into ports, and emitted as parameters and
    // file-scope variables. Left standing they reach GLSL as `in`/`out` stage
    // declarations, which in a fragment shader is a syntax error at best and
    // silently wrong at worst.
    s = cut_round(s, "in", in_port_re());
    s = cut_round(s, "out", out_port_re());
    // `use` is wiring, not code; the calls are emitted into the body. Its
    // argument list may nest parentheses, so it is cut by the same depth scan
    // that parses it rather than by a regex that would stop at the first ')'.
    {
        std::vector<std::pair<size_t, size_t>> found;
        for_each_match(s, "use", use_head_re(),
                       [&](size_t offset, const std::smatch& m) {
            size_t scan = offset + static_cast<size_t>(m.length(0));
            int open = 1;
            while (scan < s.size() && open > 0) {
                if (s[scan] == '(') ++open;
                else if (s[scan] == ')') --open;
                ++scan;
            }
            while (scan < s.size()
                   && std::isspace(static_cast<unsigned char>(s[scan]))) ++scan;
            if (scan < s.size() && s[scan] == ';') ++scan;
            found.emplace_back(offset, scan);
        });
        if (!found.empty()) {
            std::string out;
            out.reserve(s.size());
            size_t copied = 0;
            for (const auto& [begin, end] : found) {
                out.append(s, copied, begin - copied);
                copied = end;
            }
            out.append(s, copied, s.size() - copied);
            s.swap(out);
        }
    }
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

/// How deep a chain of `use` may go. A composition is inlined into one program,
/// so depth costs compile time and generated size; anything approaching this is
/// a mistake rather than a design.
constexpr int MAX_USE_DEPTH = 16;

/// Depth-first walk of a file's `use` declarations, appending each file after
/// everything it depends on. `on_stack` is the path from the root, which is
/// what makes a cycle visible and what names it when one is found.
struct CompositionWalk {
    const FjslSourceReader& read;
    FjslComposition& out;
    std::vector<std::string> on_stack;

    void report(const std::string& file, size_t line, size_t column,
                std::string message) {
        out.diagnostics.push_back({static_cast<uint32_t>(line),
                                   static_cast<uint32_t>(column),
                                   FjslSeverity::error, std::move(message), file});
    }

    /// Line and column of a byte offset in `source`, for a diagnostic about a
    /// declaration in a file the caller has in hand.
    static std::pair<size_t, size_t> position(const std::string& source, size_t offset) {
        PositionCursor cursor{source};
        auto pos = cursor.at(offset);
        return {pos.line, pos.column};
    }

    void visit(const ShaderMetadata& meta, const std::string& source,
               const std::string& path, int depth) {
        if (depth > MAX_USE_DEPTH) {
            report(path, 1, 1,
                   "composition is more than " + std::to_string(MAX_USE_DEPTH)
                       + " uses deep; this is almost certainly a mistake");
            return;
        }
        on_stack.push_back(path);

        for (const auto& use : meta.uses) {
            auto [line, column] = position(source, use.offset);

            if (std::find(on_stack.begin(), on_stack.end(), use.path) != on_stack.end()) {
                std::string chain;
                for (const auto& step : on_stack) chain += step + " -> ";
                report(path, line, column,
                       "use '" + use.name + "' is a cycle: " + chain + use.path);
                continue;
            }

            auto text = read(use.path);
            if (!text) {
                report(path, line, column,
                       "use '" + use.name + "' cannot read '" + use.path + "'");
                continue;
            }

            ShaderMetadata used = parse_fjsl(*text);
            used.source_path = use.path;

            // The used file's own problems, reported against it.
            for (const auto& d : used.diagnostics) {
                out.diagnostics.push_back({d.line, d.column, d.severity, d.message,
                                           use.path});
            }
            if (used.has_errors()) continue;

            if (!used.is_function) {
                report(path, line, column,
                       "use '" + use.name + "' names '" + use.path
                           + "', which is a shader_type "
                           + std::string(shader_type_name(used.shader_type))
                           + " shader; only a shader_function can be used");
                continue;
            }

            check_arguments(use, used, path, line, column);

            // Recurse before appending, so dependencies land first.
            visit(used, *text, use.path, depth + 1);

            const bool seen = std::any_of(
                out.files.begin(), out.files.end(),
                [&](const FjslComposedFile& f) { return f.path == use.path; });
            if (!seen) out.files.push_back({use.path, use.name, std::move(used)});
        }

        on_stack.pop_back();
    }

    /// Every `in` filled exactly once, and nothing passed that is not an `in`.
    void check_arguments(const FjslUse& use, const ShaderMetadata& used,
                         const std::string& path, size_t line, size_t column) {
        for (const auto& [name, value] : use.arguments) {
            const bool known = std::any_of(
                used.inputs.begin(), used.inputs.end(),
                [&](const FjslPort& p) { return p.name == name; });
            if (!known) {
                report(path, line, column,
                       "use '" + use.name + "': '" + use.path + "' has no in named '"
                           + name + "'");
            }
        }
        for (const auto& port : used.inputs) {
            const auto count = std::count_if(
                use.arguments.begin(), use.arguments.end(),
                [&](const auto& arg) { return arg.first == port.name; });
            if (count == 0) {
                report(path, line, column,
                       "use '" + use.name + "' does not fill in '" + port.name
                           + "'; a function takes no defaults, so every in is passed");
            } else if (count > 1) {
                report(path, line, column,
                       "use '" + use.name + "' fills in '" + port.name + "' "
                           + std::to_string(count) + " times");
            }
        }
    }
};

} // namespace

FjslComposition resolve_composition(const ShaderMetadata& meta,
                                    const std::string& path,
                                    const FjslSourceReader& read) {
    FjslComposition out;
    CompositionWalk walk{read, out, {}};
    walk.visit(meta, meta.source_text, path, 0);
    out.files.push_back({path, {}, meta});
    return out;
}

namespace {
void log_diagnostics(const std::vector<FjslDiagnostic>& diagnostics,
                     const std::string& name) {
    const std::string fallback = name.empty() ? std::string{"<fjsl>"} : name;
    for (const auto& d : diagnostics) {
        // A diagnostic carrying a file is about a used function, not the file
        // that wired it — report it where it lives.
        const std::string& file = d.file.empty() ? fallback : d.file;
        if (d.severity == FjslSeverity::error) {
            FJELL_GFX_ERROR("{}:{}:{}: error: {}", file, d.line, d.column, d.message);
        } else {
            FJELL_GFX_WARN("{}:{}:{}: warning: {}", file, d.line, d.column, d.message);
        }
    }
}
} // namespace

void log_fjsl_diagnostics(const ShaderMetadata& meta, const std::string& name) {
    log_diagnostics(meta.diagnostics, name);
}

void log_fjsl_diagnostics(const FjslComposition& composition, const std::string& name) {
    log_diagnostics(composition.diagnostics, name);
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
        // No file: this is about the source being parsed, which the caller
        // names. A composition fills it in for a used function's problems.
        meta.diagnostics.push_back({pos.line, pos.column, severity, std::move(message), {}});
    };

    // A file declares what it is, positively: `shader_type X;` for something a
    // pass draws, `shader_function;` for a pure function composed into others.
    // Never inferred from the absence of the other, so a typo stays an error
    // rather than silently becoming the other kind.
    {
        std::smatch fn;
        if (std::regex_search(source, fn, shader_function_re())) {
            meta.is_function = true;
            std::smatch clash;
            static const std::regex type_re(R"(shader_type\s+(\w+)\s*;)");
            if (std::regex_search(source, clash, type_re)) {
                diagnose(static_cast<size_t>(clash.position(0)), FjslSeverity::error,
                         "a file is either a shader_function or a shader_type, not both");
            }
        }
    }

    // Parse shader_type
    static const std::regex shader_type_re(R"(shader_type\s+(\w+)\s*;)");
    std::smatch sm;
    if (meta.is_function) {
        // Falls through to the ports and uniforms below; a function has no
        // Type, so nothing here picks a template or a material expression.
    } else if (!std::regex_search(source, sm, shader_type_re)) {
        diagnose(0, FjslSeverity::error,
                 "missing shader_type declaration; expected 'shader_type spatial;' "
                 "or 'shader_function;'");
        return meta;
    }

    if (!meta.is_function) {
        const std::string type_word = sm[1].str();
        if (auto type = shader_type_from_name(type_word)) {
            meta.shader_type = *type;
        } else {
            diagnose(static_cast<size_t>(sm.position(1)), FjslSeverity::error,
                     "unsupported shader_type '" + type_word
                         + "'; expected spatial, sky, ui, terrain or water");
            return meta;
        }
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

    uint32_t texture_slot = 4; // custom textures start at slot 4
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
            if (texture_slot > 7) {
                // Skipping leaves the name undefined in the generated GLSL, so
                // the real complaint arrives later as an undeclared identifier
                // somewhere the author cannot place. Say it here, where the
                // declaration is.
                diagnose(at + static_cast<size_t>(m.position(2)), FjslSeverity::error,
                         "uniform '" + u.name + "' is the " + std::to_string(texture_slot - 3)
                             + "th custom sampler2D; a material has room for 4");
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

    // `in` / `out` ports. Only a shader_function has them: a shader's inputs
    // are its Type's built-ins and its outputs are the template's attachments,
    // neither of which the author declares.
    auto collect_ports = [&](std::string_view keyword, const std::regex& re,
                             std::vector<FjslPort>& into) {
        for_each_match(source, keyword, re,
                       [&](size_t offset, const std::smatch& m) {
            const std::string type_name = m[1].str();
            FjslPort port;
            port.name = m[2].str();
            port.offset = offset;
            try {
                port.type = parse_type(type_name);
            } catch (const std::exception&) {
                diagnose(offset, FjslSeverity::error,
                         "'" + type_name + "' is not a type a " + std::string(keyword)
                             + " can carry; expected float, vec2, vec3 or vec4");
                return;
            }
            if (port.type == UniformType::Sampler2D) {
                diagnose(offset, FjslSeverity::error,
                         "a sampler2D cannot be passed through " + std::string(keyword)
                             + "; declare it as a uniform of the function that samples it");
                return;
            }
            if (!meta.is_function) {
                diagnose(offset, FjslSeverity::error,
                         "only a shader_function declares " + std::string(keyword)
                             + " ports");
                return;
            }
            into.push_back(std::move(port));
        });
    };
    collect_ports("in", in_port_re(), meta.inputs);
    collect_ports("out", out_port_re(), meta.outputs);

    // `use` wirings, in source order.
    for_each_match(source, "use", use_head_re(),
                   [&](size_t offset, const std::smatch& m) {
        FjslUse use;
        use.name = m[1].str();
        use.path = m[2].str();
        use.offset = offset;
        // The match ends just past the opening `(`. Walk to the `)` that
        // closes it, counting depth, so an argument may be any expression.
        size_t scan = offset + static_cast<size_t>(m.length(0));
        int open = 1;
        const size_t args_begin = scan;
        while (scan < source.size() && open > 0) {
            if (source[scan] == '(') ++open;
            else if (source[scan] == ')') --open;
            ++scan;
        }
        if (open != 0) {
            diagnose(offset, FjslSeverity::error,
                     "use '" + use.name + "' has no closing ')'");
            return;
        }
        {
            size_t semi = scan;
            while (semi < source.size()
                   && std::isspace(static_cast<unsigned char>(source[semi]))) ++semi;
            if (semi >= source.size() || source[semi] != ';') {
                diagnose(offset, FjslSeverity::error,
                         "use '" + use.name + "' needs a ';' after it");
                return;
            }
        }
        // `view_dir: VIEW_DIR, sun_dir: SUN_DIR` — split on the commas between
        // arguments, not the ones inside an expression's own call.
        const std::string args = source.substr(args_begin, scan - 1 - args_begin);
        std::string current;
        int depth = 0;
        auto take = [&] {
            const std::string text = trim(current);
            current.clear();
            if (text.empty()) return;
            const auto colon = text.find(':');
            if (colon == std::string::npos) {
                diagnose(offset, FjslSeverity::error,
                         "argument '" + text + "' to use '" + use.name
                             + "' needs the name of the in it fills, as 'name: value'");
                return;
            }
            use.arguments.emplace_back(trim(text.substr(0, colon)),
                                       trim(text.substr(colon + 1)));
        };
        for (char c : args) {
            if (c == '(' || c == '[') ++depth;
            else if (c == ')' || c == ']') --depth;
            if (c == ',' && depth == 0) { take(); continue; }
            current += c;
        }
        take();
        meta.uses.push_back(std::move(use));
    });

    // Sky shaders supply their user code via sky() rather than fragment();
    // route it through the same fragment_body slot so the existing
    // /*FJSL_USER_CODE*/ substitution path works unchanged.
    if (meta.is_function) {
        meta.main_body = extract_function_body(source, "main");
    } else {
        meta.fragment_body = meta.shader_type == ShaderType::Sky
            ? extract_function_body(source, "sky")
            : extract_function_body(source, "fragment");
        meta.vertex_body = extract_function_body(source, "vertex");
        meta.light_body = extract_function_body(source, "light");
        meta.layers_body = extract_function_body(source, "layers");
    }
    meta.user_globals = extract_user_globals(source, meta.type());
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
    const std::string kind =
        meta.is_function ? std::string("a shader_function")
                         : "shader_type " + std::string(shader_type_name(meta.shader_type));
    for (const auto& rule : hook_rules(meta.type())) {
        if (rule.refusal.empty()) continue;

        const size_t at = find_function_declaration(source, rule.hook);
        if (at == std::string::npos) continue;

        diagnose(at, FjslSeverity::error,
                 kind + " has no " + std::string(rule.hook) + "() stage — "
                     + std::string(rule.refusal));
    }

    if (meta.is_function) {
        if (meta.main_body.empty()) {
            diagnose(0, FjslSeverity::error,
                     "a shader_function needs a main() to run");
        }
        if (meta.outputs.empty()) {
            diagnose(0, FjslSeverity::error,
                     "a shader_function with no out declares nothing a caller can read");
        }
        // A duplicate port name would silently shadow: both mangle to the same
        // identifier in the composed shader.
        auto check_unique = [&](const std::vector<FjslPort>& ports, const char* what) {
            for (size_t i = 0; i < ports.size(); ++i) {
                for (size_t j = 0; j < i; ++j) {
                    if (ports[i].name != ports[j].name) continue;
                    diagnose(ports[i].offset, FjslSeverity::error,
                             std::string(what) + " '" + ports[i].name
                                 + "' is declared twice");
                }
            }
        };
        check_unique(meta.inputs, "in");
        check_unique(meta.outputs, "out");
        for (const auto& out : meta.outputs) {
            for (const auto& in : meta.inputs) {
                if (out.name != in.name) continue;
                diagnose(out.offset, FjslSeverity::error,
                         "'" + out.name + "' is both an in and an out");
            }
        }
    }

    // A `use` names a file under a local name; two of the same name would
    // mangle their contents together.
    for (size_t i = 0; i < meta.uses.size(); ++i) {
        for (size_t j = 0; j < i; ++j) {
            if (meta.uses[i].name != meta.uses[j].name) continue;
            diagnose(meta.uses[i].offset, FjslSeverity::error,
                     "use '" + meta.uses[i].name + "' is declared twice");
        }
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

} // namespace

namespace {

/// Prefix every identifier a composed file declares, so two functions that
/// both call a helper `noise` or declare a uniform `size` do not collide.
///
/// This is not cosmetic. A uniform becomes a file-scope `#define`, and a macro
/// has no scope: two `#define size` leave the second winning for everything
/// after it, including the first function's own body. Globals are worse — two
/// `float noise(vec2)` at file scope is a redefinition GLSL refuses outright.
std::string mangled(std::string_view use_name, std::string_view identifier) {
    // One underscore, not two: GLSL reserves identifiers containing consecutive
    // underscores, and glslc warns on every one. A use name cannot contain an
    // underscore-digit sequence that would make this ambiguous, because the
    // suffix is always a whole declared name.
    return std::string(use_name) + "_" + std::string(identifier);
}

/// Rewrite whole-word occurrences of each name in `text`.
///
/// Identifier boundaries only: renaming `size` must not touch `size_of` or the
/// `size` inside `point_size`. Rewrites in one pass so a name introduced by an
/// earlier replacement is never itself replaced.
std::string rename_identifiers(const std::string& text,
                               const std::map<std::string, std::string>& renames) {
    if (renames.empty()) return text;
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        if (!is_identifier_char(text[i])) {
            out += text[i++];
            continue;
        }
        size_t end = i;
        while (end < text.size() && is_identifier_char(text[end])) ++end;
        const std::string word = text.substr(i, end - i);
        auto it = renames.find(word);
        out += (it == renames.end()) ? word : it->second;
        i = end;
    }
    return out;
}

/// Every name a composed file declares, mapped to its mangled form: its
/// uniforms, its ports, and the file-scope functions and constants in its
/// globals.
std::map<std::string, std::string> composition_renames(const ShaderMetadata& meta,
                                                       std::string_view use_name) {
    std::map<std::string, std::string> renames;
    for (const auto& u : meta.uniforms) renames[u.name] = mangled(use_name, u.name);
    for (const auto& p : meta.inputs)   renames[p.name] = mangled(use_name, p.name);
    for (const auto& p : meta.outputs)  renames[p.name] = mangled(use_name, p.name);

    // A top-level declaration in the globals blob: `<type> <name>(` for a
    // function, `<type> <name> =` or `;` for a constant, with an optional
    // `const` and precision qualifier before the type. Verified against every
    // global in the shader library — the `const` alternative is load-bearing,
    // since `const mat2 M = ...` puts three tokens before the name.
    static const std::regex decl(
        R"(^(?:const\s+)?(?:lowp\s+|mediump\s+|highp\s+)?(\w+)\s+(\w+)\s*[\(=;\[])",
        std::regex::multiline);
    for (auto it = std::sregex_iterator(meta.user_globals.begin(),
                                        meta.user_globals.end(), decl);
         it != std::sregex_iterator(); ++it) {
        const std::string name = (*it)[2].str();
        renames[name] = mangled(use_name, name);
    }
    return renames;
}

/// Rewrite a consumer's `use`-qualified references into the mangled names the
/// emitted functions actually declare: `sun.color` reads `sun__color`, and the
/// wiring itself becomes a call to `sun__main(...)` placed where the body can
/// see it.
///
/// Only a name a `use` introduced is touched, so `v.xyz` on an ordinary vector
/// is left alone — the swizzle is not a use name.
std::string rewrite_use_references(const std::string& body,
                                   const std::vector<FjslUse>& uses) {
    if (uses.empty()) return body;
    std::string out;
    out.reserve(body.size());
    size_t i = 0;
    while (i < body.size()) {
        if (!is_identifier_char(body[i])) {
            out += body[i++];
            continue;
        }
        size_t end = i;
        while (end < body.size() && is_identifier_char(body[end])) ++end;
        const std::string word = body.substr(i, end - i);

        // `<use>.<member>` only; a bare use name on its own is not a value.
        size_t dot = end;
        while (dot < body.size() && (body[dot] == ' ' || body[dot] == '\t')) ++dot;
        const bool qualified = dot < body.size() && body[dot] == '.';
        const bool is_use = std::any_of(uses.begin(), uses.end(),
                                        [&](const FjslUse& u) { return u.name == word; });
        if (!qualified || !is_use) {
            out += word;
            i = end;
            continue;
        }
        size_t member = dot + 1;
        while (member < body.size()
               && (body[member] == ' ' || body[member] == '\t')) ++member;
        size_t member_end = member;
        while (member_end < body.size() && is_identifier_char(body[member_end])) ++member_end;
        out += mangled(word, body.substr(member, member_end - member));
        i = member_end;
    }
    return out;
}

/// The calls that run each used function, in composition order, ahead of the
/// consumer's own body.
std::string emit_use_calls(const std::vector<FjslUse>& uses,
                           const FjslComposition& composition) {
    std::string out;
    for (const auto& use : uses) {
        // Arguments in the order the function declares its ins, not the order
        // they were written.
        const ShaderMetadata* used = nullptr;
        for (const auto& f : composition.files) {
            if (f.path == use.path) { used = &f.meta; break; }
        }
        if (used == nullptr) continue;

        out += "    " + mangled(use.name, "main") + "(";
        for (size_t i = 0; i < used->inputs.size(); ++i) {
            if (i > 0) out += ", ";
            for (const auto& [name, value] : use.arguments) {
                if (name != used->inputs[i].name) continue;
                out += value;
                break;
            }
        }
        out += ");\n";
    }
    return out;
}

/// A composed function, emitted at file scope: its globals, its uniforms'
/// defines, its outputs as variables, and its body wrapped in a function the
/// consumer calls.
std::string emit_composed_file(const FjslComposedFile& file,
                               const std::string& mat_expr,
                               uint32_t param_base) {
    const ShaderMetadata& m = file.meta;
    const std::string use = file.use_name;
    const auto renames = composition_renames(m, use);

    std::string out = "// ── " + file.path + " (as '" + use + "') ──\n";

    // Uniform defines, reading this file's OWN material row: a texture it
    // declares lives in its row, not the consumer's, so the four custom
    // sampler slots are never shared across a composition.
    for (const auto& u : m.uniforms) {
        const std::string name = mangled(use, u.name);
        if (u.type == UniformType::Sampler2D) {
            out += "#define " + name + " textures[nonuniformEXT(" + mat_expr
                 + ".texture_indices[" + std::to_string(u.slot) + "])]\n";
            continue;
        }
        const uint32_t count = uniform_float_count(u.type);
        std::string type_str;
        switch (u.type) {
            case UniformType::Float: type_str = "float"; break;
            case UniformType::Vec2:  type_str = "vec2";  break;
            case UniformType::Vec3:  type_str = "vec3";  break;
            case UniformType::Vec4:  type_str = "vec4";  break;
            default: break;
        }
        // A composed file's uniforms are inlined into the CONSUMER's param
        // block, so its own slots — which its parse numbered from zero — are
        // shifted by where that file's run begins. Without this every file's
        // first uniform reads the consumer's first, silently.
        out += "#define " + name + " (";
        if (count == 1) {
            out += "custom_params[" + mat_expr + ".param_offset + "
                 + std::to_string(param_base + u.slot) + "]";
        } else {
            out += type_str + "(";
            for (uint32_t i = 0; i < count; ++i) {
                if (i > 0) out += ", ";
                out += "custom_params[" + mat_expr + ".param_offset + "
                     + std::to_string(param_base + u.slot + i) + "]";
            }
            out += ")";
        }
        out += ")\n";
    }

    // The file's own helpers and constants, renamed.
    if (!m.user_globals.empty()) {
        out += rename_identifiers(m.user_globals, renames) + "\n";
    }

    // Outputs are file-scope variables the consumer reads after the call.
    for (const auto& p : m.outputs) {
        out += std::string(glsl_type_name(p.type)) + " " + mangled(use, p.name) + ";\n";
    }

    // main() becomes a named function taking the ins as parameters.
    out += "void " + mangled(use, "main") + "(";
    for (size_t i = 0; i < m.inputs.size(); ++i) {
        if (i > 0) out += ", ";
        out += std::string(glsl_type_name(m.inputs[i].type)) + " "
             + mangled(use, m.inputs[i].name);
    }
    out += ") {\n";
    out += rename_identifiers(m.main_body.code, renames);
    out += "\n}\n";
    return out;
}

} // namespace

std::string generate_fragment_shader(const ShaderMetadata& meta,
                                     const std::string& template_source,
                                     const FjslComposition* composition) {
    std::string result = template_source;
    const std::string generated_name = GENERATED_NAME;

    // Generate custom texture defines
    std::string tex_defines;
    for (const auto& u : meta.uniforms) {
        if (u.type == UniformType::Sampler2D) {
            tex_defines += "#define " + u.name +
                           " textures[nonuniformEXT(mat.texture_indices[" +
                           std::to_string(u.slot) + "])]\n";
        }
    }

    // Generate custom param reads as file-scope #defines so they're visible
    // inside fragment(), light(), and any user helper function — not just
    // inside main(). Each macro reads from the materials SSBO using
    // frag_draw_index, which is in scope throughout the fragment shader.
    std::string param_defines;
    // Each shader type reaches its material row differently. Sky is a single
    // fullscreen pass with no draws[] SSBO, so the row index is pushed as
    // `push.sky_material_id`; terrain and water likewise push their row per
    // node; a ui quad carries its row in the quad record; spatial shaders
    // look up the material via per-draw data.
    const std::string mat_expr = [&]() -> std::string {
        switch (meta.shader_type) {
            case ShaderType::Sky:     return "materials[push.sky_material_id]";
            case ShaderType::Terrain: return "materials[push.terrain_material_id]";
            case ShaderType::Water:   return "materials[push.water_material_id]";
            case ShaderType::Ui:      return "materials[quads[frag_quad_index].material_id]";
            case ShaderType::Spatial: break;
        }
        return "materials[draws[frag_draw_index].material_id]";
    }();
    for (const auto& u : meta.uniforms) {
        if (u.type == UniformType::Sampler2D) continue;

        std::string type_str;
        switch (u.type) {
            case UniformType::Float: type_str = "float"; break;
            case UniformType::Vec2:  type_str = "vec2";  break;
            case UniformType::Vec3:  type_str = "vec3";  break;
            case UniformType::Vec4:  type_str = "vec4";  break;
            default: break;
        }

        uint32_t count = uniform_float_count(u.type);
        param_defines += "#define " + u.name + " (";
        if (count == 1) {
            param_defines += "custom_params[" + mat_expr + ".param_offset + " +
                             std::to_string(u.slot) + "]";
        } else {
            param_defines += type_str + "(";
            for (uint32_t i = 0; i < count; ++i) {
                if (i > 0) param_defines += ", ";
                param_defines += "custom_params[" + mat_expr + ".param_offset + " +
                                 std::to_string(u.slot + i) + "]";
            }
            param_defines += ")";
        }
        param_defines += ")\n";
    }

    // Texture defines must also be moved to file scope, since the existing
    // `mat` local is only defined inside main(). Rewrite them to the same
    // frag_draw_index lookup so they work in light()/helpers too.
    std::string tex_defines_global;
    for (const auto& u : meta.uniforms) {
        if (u.type == UniformType::Sampler2D) {
            tex_defines_global += "#define " + u.name +
                " textures[nonuniformEXT(" + mat_expr + ".texture_indices[" +
                std::to_string(u.slot) + "])]\n";
        }
    }
    tex_defines = tex_defines_global;

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
        // `sun.color` in the authored body names a variable the emitted
        // function declares as `sun__color`, and the call that fills it runs
        // ahead of the body. Both only happen where a `use` introduced the
        // name, so ordinary member access is untouched.
        FjslBody rewritten = body;
        std::string calls;
        if (composition != nullptr && !meta.uses.empty()) {
            rewritten.code = rewrite_use_references(body.code, meta.uses);
            calls = emit_use_calls(meta.uses, *composition);
        }
        std::string text = calls + with_line_markers(rewritten, meta)
                         + "#line " + std::to_string(resume)
                         + " \"" + generated_name + "\"\n";
        result.replace(pos, placeholder.size(), text);
    };

    replace("/*FJSL_CUSTOM_TEXTURES*/", tex_defines + param_defines);
    // The CUSTOM_PARAMS placeholder originally injected local reads inside
    // main(); now param values come from the file-scope #defines emitted
    // above, so the in-main slot becomes empty.
    replace("/*FJSL_CUSTOM_PARAMS*/", std::string{});
    // Composed functions go in ahead of the consumer's own globals, at file
    // scope and above main() — the unlit path finds main()'s closing brace with
    // rfind('}'), so nothing may be emitted after it. The composition lists
    // dependencies first, so a function's helpers are declared before the code
    // that calls them.
    // Each composed file's uniforms occupy a run of the consumer's param
    // block, after the consumer's own. The consumer was numbered from zero by
    // its parse, so the first run starts where its count ends.
    std::string globals;
    if (composition != nullptr) {
        uint32_t param_base = meta.custom_param_count;
        for (const auto& file : composition->files) {
            if (file.use_name.empty()) continue;  // the consumer itself
            globals += emit_composed_file(file, mat_expr, param_base) + "\n";
            param_base += file.meta.custom_param_count;
        }
    }
    globals += meta.user_globals;
    replace("/*FJSL_USER_GLOBALS*/", globals);
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
    // The default body is engine code, so it gets no #line — an error in it is
    // an engine bug and should point at the generated file.
    if (meta.light_body.empty()) {
        replace("/*FJSL_LIGHT_USER_CODE*/", std::string(DEFAULT_LIGHT_BODY));
    } else {
        replace_body("/*FJSL_LIGHT_USER_CODE*/", meta.light_body);
    }

    // Replace PBR lighting block with direct output for unlit shaders
    if (meta.render_mode == RenderMode::Unlit) {
        const std::string lighting_marker = "    // \xe2\x94\x80\xe2\x94\x80 PBR lighting";
        auto lighting_pos = result.find(lighting_marker);
        if (lighting_pos != std::string::npos) {
            // Find the closing brace of main() — last '}' in the file
            auto main_end = result.rfind('}');
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

    // Generate custom param reads (same SSBO layout as fragment)
    std::string param_reads;
    for (const auto& u : meta.uniforms) {
        if (u.type == UniformType::Sampler2D) continue;

        std::string type_str;
        switch (u.type) {
            case UniformType::Float: type_str = "float"; break;
            case UniformType::Vec2: type_str = "vec2"; break;
            case UniformType::Vec3: type_str = "vec3"; break;
            case UniformType::Vec4: type_str = "vec4"; break;
            default: break;
        }

        uint32_t count = uniform_float_count(u.type);
        if (count == 1) {
            param_reads += "    " + type_str + " " + u.name +
                           " = custom_params[mat.param_offset + " +
                           std::to_string(u.slot) + "];\n";
        } else {
            param_reads += "    " + type_str + " " + u.name + " = " + type_str + "(";
            for (uint32_t i = 0; i < count; ++i) {
                if (i > 0) param_reads += ", ";
                param_reads += "custom_params[mat.param_offset + " +
                               std::to_string(u.slot + i) + "]";
            }
            param_reads += ");\n";
        }
    }

    auto replace = [&](const std::string& placeholder, const std::string& replacement) {
        auto pos = result.find(placeholder);
        if (pos != std::string::npos) {
            result.replace(pos, placeholder.size(), replacement);
        }
    };

    replace("/*FJSL_VERTEX_CUSTOM_PARAMS*/", param_reads);

    // The same body is substituted into all six vertex/mesh templates, so a
    // #line here means one authored line maps to the right place in every one
    // of them rather than to six different generated lines.
    {
        auto pos = result.find("/*FJSL_VERTEX_USER_CODE*/");
        if (pos != std::string::npos && !meta.vertex_body.empty()) {
            const uint32_t resume = line_at(result, pos) + 1;
            result.replace(pos, std::string("/*FJSL_VERTEX_USER_CODE*/").size(),
                           with_line_markers(meta.vertex_body, meta)
                               + "#line " + std::to_string(resume)
                               + " \"" + GENERATED_NAME + "\"\n");
        } else {
            replace("/*FJSL_VERTEX_USER_CODE*/", meta.vertex_body.code);
        }
    }

    enable_line_directives(result);
    return result;
}

} // namespace fjell
