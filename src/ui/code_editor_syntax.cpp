#include "ui/code_editor_syntax.hpp"
#include "ui/theme.hpp"

#include <algorithm>
#include <cctype>

namespace fjell {

// ---------------------------------------------------------------------------
// Token colors (sRGB → linear for Vulkan sRGB framebuffer)
// ---------------------------------------------------------------------------

static ImU32 srgb_u32(int r, int g, int b, int a = 255) {
    auto to_lin = [](int v) -> int {
        float s = static_cast<float>(v) / 255.0f;
        return static_cast<int>(theme::srgb_to_linear(s) * 255.0f + 0.5f);
    };
    return IM_COL32(to_lin(r), to_lin(g), to_lin(b), a);
}

ImU32 token_color(TokenType type) {
    // Colors specified in sRGB, converted to linear at first call
    static const ImU32 DEFAULT      = srgb_u32(220, 223, 228); // #DCE0E4
    static const ImU32 KEYWORD      = srgb_u32(198, 120, 221); // #C678DD purple
    static const ImU32 TYPE         = srgb_u32( 86, 182, 194); // #56B6C2 cyan
    static const ImU32 BUILTIN      = srgb_u32(209, 154,  98); // #D19A62 warm orange
    static const ImU32 STRING       = srgb_u32(152, 195, 121); // #98C379 green
    static const ImU32 NUMBER       = srgb_u32(209, 154,  98); // #D19A62 orange
    static const ImU32 COMMENT      = srgb_u32( 92,  99, 112); // #5C6370 gray
    static const ImU32 PREPROC      = srgb_u32(198, 120, 221); // #C678DD purple
    static const ImU32 PUNCT        = srgb_u32(171, 178, 191); // #ABB2BF dim white
    static const ImU32 ATTR         = srgb_u32(209, 154,  98); // #D19A62 orange
    static const ImU32 TAG          = srgb_u32(224, 108, 117); // #E06C75 red
    static const ImU32 BIND         = srgb_u32( 86, 182, 194); // #56B6C2 cyan
    static const ImU32 SEL          = srgb_u32(224, 108, 117); // #E06C75 red

    switch (type) {
        case TokenType::Default:      return DEFAULT;
        case TokenType::Keyword:      return KEYWORD;
        case TokenType::Type:         return TYPE;
        case TokenType::Builtin:      return BUILTIN;
        case TokenType::String:       return STRING;
        case TokenType::Number:       return NUMBER;
        case TokenType::Comment:      return COMMENT;
        case TokenType::Preprocessor: return PREPROC;
        case TokenType::Punctuation:  return PUNCT;
        case TokenType::Attribute:    return ATTR;
        case TokenType::Tag:          return TAG;
        case TokenType::Binding:      return BIND;
        case TokenType::Selector:     return SEL;
    }
    return DEFAULT;
}

// ---------------------------------------------------------------------------
// Language detection
// ---------------------------------------------------------------------------

Language detect_language(std::string_view path) {
    auto dot = path.rfind('.');
    if (dot == std::string_view::npos) return Language::PlainText;

    auto ext = path.substr(dot);
    if (ext == ".cpp" || ext == ".hpp" || ext == ".h" || ext == ".cc" || ext == ".cxx")
        return Language::Cpp;
    if (ext == ".vert" || ext == ".frag" || ext == ".comp" || ext == ".glsl" ||
        ext == ".mesh" || ext == ".task")
        return Language::Glsl;
    if (ext == ".fjsl") return Language::Fjsl;
    if (ext == ".fjui") return Language::Fjui;
    if (ext == ".fjss") return Language::Fjss;
    if (ext == ".json" || ext == ".fjmat") return Language::Json;
    return Language::PlainText;
}

// ---------------------------------------------------------------------------
// Keyword sets
// ---------------------------------------------------------------------------

static const std::unordered_set<std::string>& cpp_keywords() {
    static const std::unordered_set<std::string> kw = {
        "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor",
        "break", "case", "catch", "class", "co_await", "co_return", "co_yield",
        "compl", "concept", "const", "consteval", "constexpr", "constinit",
        "const_cast", "continue", "decltype", "default", "delete", "do",
        "dynamic_cast", "else", "enum", "explicit", "export", "extern", "false",
        "for", "friend", "goto", "if", "inline", "mutable", "namespace", "new",
        "noexcept", "not", "not_eq", "nullptr", "operator", "or", "or_eq",
        "override", "private", "protected", "public", "register",
        "reinterpret_cast", "requires", "return", "sizeof", "static",
        "static_assert", "static_cast", "struct", "switch", "template", "this",
        "throw", "true", "try", "typedef", "typeid", "typename", "union",
        "using", "virtual", "volatile", "while", "xor", "xor_eq",
        "final", "import", "module",
    };
    return kw;
}

static const std::unordered_set<std::string>& cpp_types() {
    static const std::unordered_set<std::string> ty = {
        "bool", "char", "char8_t", "char16_t", "char32_t", "double", "float",
        "int", "long", "short", "signed", "unsigned", "void", "wchar_t",
        "int8_t", "int16_t", "int32_t", "int64_t",
        "uint8_t", "uint16_t", "uint32_t", "uint64_t",
        "size_t", "ptrdiff_t", "intptr_t", "uintptr_t",
        "string", "string_view", "vector", "array", "map", "unordered_map",
        "set", "unordered_set", "unique_ptr", "shared_ptr", "weak_ptr",
        "optional", "variant", "tuple", "pair", "span", "filesystem",
    };
    return ty;
}

static const std::unordered_set<std::string>& glsl_keywords() {
    static const std::unordered_set<std::string> kw = {
        "attribute", "break", "case", "const", "continue", "default", "discard",
        "do", "else", "flat", "for", "highp", "if", "in", "inout", "layout",
        "lowp", "mediump", "noperspective", "out", "precision", "return",
        "smooth", "struct", "switch", "uniform", "varying", "while",
        "buffer", "shared", "coherent", "volatile", "restrict", "readonly",
        "writeonly", "subroutine",
        "true", "false",
    };
    return kw;
}

static const std::unordered_set<std::string>& glsl_types() {
    static const std::unordered_set<std::string> ty = {
        "void", "bool", "int", "uint", "float", "double",
        "vec2", "vec3", "vec4", "dvec2", "dvec3", "dvec4",
        "bvec2", "bvec3", "bvec4", "ivec2", "ivec3", "ivec4",
        "uvec2", "uvec3", "uvec4",
        "mat2", "mat3", "mat4", "mat2x2", "mat2x3", "mat2x4",
        "mat3x2", "mat3x3", "mat3x4", "mat4x2", "mat4x3", "mat4x4",
        "sampler1D", "sampler2D", "sampler3D", "samplerCube",
        "sampler2DShadow", "sampler2DArray", "image2D",
    };
    return ty;
}

static const std::unordered_set<std::string>& glsl_builtins() {
    static const std::unordered_set<std::string> bi = {
        "gl_Position", "gl_FragCoord", "gl_VertexIndex", "gl_InstanceIndex",
        "gl_FragDepth", "gl_PointSize", "gl_FrontFacing", "gl_GlobalInvocationID",
        "gl_LocalInvocationID", "gl_WorkGroupID", "gl_NumWorkGroups",
        "gl_LocalInvocationIndex", "gl_WorkGroupSize",
        // Common functions
        "texture", "textureLod", "textureProj", "texelFetch",
        "mix", "clamp", "smoothstep", "step",
        "normalize", "dot", "cross", "length", "distance", "reflect", "refract",
        "min", "max", "abs", "sign", "floor", "ceil", "fract", "mod", "pow",
        "sqrt", "inversesqrt", "exp", "exp2", "log", "log2",
        "radians", "degrees", "sin", "cos", "tan", "asin", "acos", "atan",
        "transpose", "inverse", "determinant",
        "dFdx", "dFdy", "fwidth",
        "barrier", "memoryBarrier", "groupMemoryBarrier",
        "imageLoad", "imageStore",
    };
    return bi;
}

static const std::unordered_set<std::string>& fjsl_keywords() {
    static const std::unordered_set<std::string> kw = {
        "shader_type", "spatial", "render_mode",
        "unlit", "cull_disabled", "cull_front", "cull_back",
        "depth_write_disabled", "depth_test_disabled",
        "blend_add", "blend_multiply",
        "hint_color", "hint_white", "hint_black", "hint_normal",
        "hint_range", "hint_default_mr",
    };
    return kw;
}

static const std::unordered_set<std::string>& fjsl_builtins() {
    static const std::unordered_set<std::string> bi = {
        "ALBEDO", "ROUGHNESS", "METALLIC", "NORMAL", "EMISSION", "AO", "ALPHA",
        "VERTEX", "UV", "MODEL_MATRIX", "TIME", "VIEW_DIR",
        "fragment", "vertex",
    };
    return bi;
}

static const std::unordered_set<std::string>& fjui_tags() {
    static const std::unordered_set<std::string> tags = {
        "panel", "div", "text", "bar", "img", "input", "button", "slot",
    };
    return tags;
}

static const std::unordered_set<std::string>& fjss_properties() {
    static const std::unordered_set<std::string> props = {
        "width", "height", "min-width", "min-height", "max-width", "max-height",
        "padding", "padding-top", "padding-bottom", "padding-left", "padding-right",
        "margin", "margin-top", "margin-bottom", "margin-left", "margin-right",
        "gap", "flex-direction", "flex-wrap", "flex-grow", "flex-shrink",
        "justify-content", "align-items", "align-self",
        "position", "top", "bottom", "left", "right",
        "background-color", "background", "color", "opacity",
        "border", "border-color", "border-radius",
        "box-shadow", "overflow",
        "font-size", "text-align",
        "transition",
        "display",
    };
    return props;
}

static const std::unordered_set<std::string>& fjss_values() {
    static const std::unordered_set<std::string> vals = {
        "row", "column", "row-reverse", "column-reverse",
        "center", "flex-start", "flex-end", "space-between", "space-around",
        "stretch", "baseline", "wrap", "nowrap",
        "absolute", "relative",
        "solid", "none", "hidden", "scroll", "auto",
        "ease-in-out", "ease-in", "ease-out", "linear",
        "left", "right", "center",
    };
    return vals;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool is_ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

static bool is_ident_start(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

// Unused for now, but may be needed for future token boundary checks
// static bool is_word_boundary(std::string_view line, int start, int end) {
//     if (start > 0 && is_ident_char(line[start - 1])) return false;
//     if (end < static_cast<int>(line.size()) && is_ident_char(line[end])) return false;
//     return true;
// }

// ---------------------------------------------------------------------------
// C-family tokenizer (shared by C++, GLSL, FJSL)
// ---------------------------------------------------------------------------

struct CFamilyConfig {
    const std::unordered_set<std::string>* keywords;
    const std::unordered_set<std::string>* types;
    const std::unordered_set<std::string>* builtins;
    bool has_preprocessor;
};

static LineState tokenize_c_family(
    std::string_view line, LineState prev, std::vector<Token>& out,
    const CFamilyConfig& cfg)
{
    LineState next{};
    int i = 0;
    int len = static_cast<int>(line.size());

    // If we're continuing a block comment from the previous line
    if (prev.in_block_comment) {
        int start = 0;
        while (i < len - 1) {
            if (line[i] == '*' && line[i + 1] == '/') {
                i += 2;
                out.push_back({start, i, TokenType::Comment});
                goto after_block_comment;
            }
            ++i;
        }
        // Check last char
        if (i < len) ++i;
        out.push_back({start, len, TokenType::Comment});
        next.in_block_comment = true;
        return next;
    }

after_block_comment:

    while (i < len) {
        char c = line[i];

        // Skip whitespace
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }

        // Line comment
        if (c == '/' && i + 1 < len && line[i + 1] == '/') {
            out.push_back({i, len, TokenType::Comment});
            return next;
        }

        // Block comment start
        if (c == '/' && i + 1 < len && line[i + 1] == '*') {
            int start = i;
            i += 2;
            while (i < len - 1) {
                if (line[i] == '*' && line[i + 1] == '/') {
                    i += 2;
                    out.push_back({start, i, TokenType::Comment});
                    goto after_block_comment; // NOLINT
                }
                ++i;
            }
            if (i < len) ++i;
            out.push_back({start, len, TokenType::Comment});
            next.in_block_comment = true;
            return next;
        }

        // Preprocessor
        if (cfg.has_preprocessor && c == '#') {
            out.push_back({i, len, TokenType::Preprocessor});
            return next;
        }

        // String literal
        if (c == '"' || c == '\'') {
            char quote = c;
            int start = i;
            ++i;
            while (i < len) {
                if (line[i] == '\\' && i + 1 < len) {
                    i += 2; // skip escape
                    continue;
                }
                if (line[i] == quote) {
                    ++i;
                    break;
                }
                ++i;
            }
            out.push_back({start, i, TokenType::String});
            continue;
        }

        // Number
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && i + 1 < len && std::isdigit(static_cast<unsigned char>(line[i + 1])))) {
            int start = i;
            // Hex prefix
            if (c == '0' && i + 1 < len && (line[i + 1] == 'x' || line[i + 1] == 'X')) {
                i += 2;
                while (i < len && std::isxdigit(static_cast<unsigned char>(line[i]))) ++i;
            } else {
                while (i < len && (std::isdigit(static_cast<unsigned char>(line[i])) ||
                                   line[i] == '.' || line[i] == 'e' || line[i] == 'E' ||
                                   line[i] == 'f' || line[i] == 'F' ||
                                   line[i] == 'u' || line[i] == 'U' ||
                                   line[i] == 'l' || line[i] == 'L')) ++i;
            }
            // Consume suffixes
            while (i < len && is_ident_char(line[i])) ++i;
            out.push_back({start, i, TokenType::Number});
            continue;
        }

        // Identifier / keyword
        if (is_ident_start(c)) {
            int start = i;
            while (i < len && is_ident_char(line[i])) ++i;
            std::string word(line.substr(start, i - start));

            if (cfg.keywords && cfg.keywords->contains(word)) {
                out.push_back({start, i, TokenType::Keyword});
            } else if (cfg.types && cfg.types->contains(word)) {
                out.push_back({start, i, TokenType::Type});
            } else if (cfg.builtins && cfg.builtins->contains(word)) {
                out.push_back({start, i, TokenType::Builtin});
            } else {
                out.push_back({start, i, TokenType::Default});
            }
            continue;
        }

        // Punctuation (single char)
        out.push_back({i, i + 1, TokenType::Punctuation});
        ++i;
    }

    return next;
}

// ---------------------------------------------------------------------------
// FJUI tokenizer (HTML-like)
// ---------------------------------------------------------------------------

static LineState tokenize_fjui(
    std::string_view line, LineState prev, std::vector<Token>& out)
{
    LineState next{};
    int i = 0;
    int len = static_cast<int>(line.size());

    // Block comment continuation (<!-- ... -->)
    if (prev.in_block_comment) {
        int start = 0;
        while (i < len - 2) {
            if (line[i] == '-' && line[i + 1] == '-' && line[i + 2] == '>') {
                i += 3;
                out.push_back({start, i, TokenType::Comment});
                goto fjui_after_comment;
            }
            ++i;
        }
        i = len;
        out.push_back({start, len, TokenType::Comment});
        next.in_block_comment = true;
        return next;
    }

fjui_after_comment:

    while (i < len) {
        char c = line[i];

        // HTML comment <!-- -->
        if (c == '<' && i + 3 < len && line[i + 1] == '!' &&
            line[i + 2] == '-' && line[i + 3] == '-') {
            int start = i;
            i += 4;
            while (i < len - 2) {
                if (line[i] == '-' && line[i + 1] == '-' && line[i + 2] == '>') {
                    i += 3;
                    out.push_back({start, i, TokenType::Comment});
                    goto fjui_after_comment; // NOLINT
                }
                ++i;
            }
            i = len;
            out.push_back({start, len, TokenType::Comment});
            next.in_block_comment = true;
            return next;
        }

        // Data binding {key}
        if (c == '{') {
            int start = i;
            ++i;
            while (i < len && line[i] != '}') ++i;
            if (i < len) ++i; // consume '}'
            out.push_back({start, i, TokenType::Binding});
            continue;
        }

        // Tag open/close
        if (c == '<') {
            int start = i;
            ++i;
            bool closing = (i < len && line[i] == '/');
            if (closing) ++i;

            // Tag name
            int name_start = i;
            while (i < len && is_ident_char(line[i])) ++i;
            if (i > name_start) {
                // The '<' or '</' punctuation
                out.push_back({start, name_start, TokenType::Punctuation});
                std::string tag_name(line.substr(name_start, i - name_start));
                if (fjui_tags().contains(tag_name)) {
                    out.push_back({name_start, i, TokenType::Tag});
                } else {
                    out.push_back({name_start, i, TokenType::Tag});
                }
            } else {
                out.push_back({start, i, TokenType::Punctuation});
            }

            // Attributes inside the tag
            while (i < len && line[i] != '>' && !(line[i] == '/' && i + 1 < len && line[i + 1] == '>')) {
                if (std::isspace(static_cast<unsigned char>(line[i]))) {
                    ++i;
                    continue;
                }
                // Attribute name
                if (is_ident_start(line[i]) || line[i] == ':') {
                    int attr_start = i;
                    while (i < len && (is_ident_char(line[i]) || line[i] == ':' || line[i] == '-')) ++i;
                    out.push_back({attr_start, i, TokenType::Attribute});

                    // = "value"
                    if (i < len && line[i] == '=') {
                        out.push_back({i, i + 1, TokenType::Punctuation});
                        ++i;
                        if (i < len && line[i] == '"') {
                            int str_start = i;
                            ++i;
                            while (i < len && line[i] != '"') ++i;
                            if (i < len) ++i;
                            out.push_back({str_start, i, TokenType::String});
                        }
                    }
                    continue;
                }
                ++i;
            }

            // Closing > or />
            if (i < len) {
                int close_start = i;
                if (line[i] == '/' && i + 1 < len && line[i + 1] == '>') {
                    i += 2;
                } else if (line[i] == '>') {
                    ++i;
                }
                out.push_back({close_start, i, TokenType::Punctuation});
            }
            continue;
        }

        // Text content
        {
            int start = i;
            while (i < len && line[i] != '<' && line[i] != '{') ++i;
            if (i > start) {
                out.push_back({start, i, TokenType::Default});
            }
        }
    }

    return next;
}

// ---------------------------------------------------------------------------
// FJSS tokenizer (CSS-like)
// ---------------------------------------------------------------------------

static LineState tokenize_fjss(
    std::string_view line, LineState prev, std::vector<Token>& out)
{
    LineState next{};
    int i = 0;
    int len = static_cast<int>(line.size());

    // Block comment continuation
    if (prev.in_block_comment) {
        int start = 0;
        while (i < len - 1) {
            if (line[i] == '*' && line[i + 1] == '/') {
                i += 2;
                out.push_back({start, i, TokenType::Comment});
                goto fjss_after_comment;
            }
            ++i;
        }
        if (i < len) ++i;
        out.push_back({start, len, TokenType::Comment});
        next.in_block_comment = true;
        return next;
    }

fjss_after_comment:

    while (i < len) {
        char c = line[i];

        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }

        // Block comment
        if (c == '/' && i + 1 < len && line[i + 1] == '*') {
            int start = i;
            i += 2;
            while (i < len - 1) {
                if (line[i] == '*' && line[i + 1] == '/') {
                    i += 2;
                    out.push_back({start, i, TokenType::Comment});
                    goto fjss_after_comment; // NOLINT
                }
                ++i;
            }
            if (i < len) ++i;
            out.push_back({start, len, TokenType::Comment});
            next.in_block_comment = true;
            return next;
        }

        // Selector: .class, #id, :pseudo, tag
        if (c == '.' || c == '#' || c == ':') {
            int start = i;
            ++i;
            while (i < len && (is_ident_char(line[i]) || line[i] == '-')) ++i;
            out.push_back({start, i, TokenType::Selector});
            continue;
        }

        // Color hex literal
        if (c == '#' || (c == '#' && i + 1 < len && std::isxdigit(static_cast<unsigned char>(line[i + 1])))) {
            // Already handled by selector above for #id, but if inside a value context
            // this won't re-trigger. For simplicity, #hex in values is handled as number below.
        }

        // String
        if (c == '"' || c == '\'') {
            char quote = c;
            int start = i;
            ++i;
            while (i < len && line[i] != quote) ++i;
            if (i < len) ++i;
            out.push_back({start, i, TokenType::String});
            continue;
        }

        // Number (with optional unit-like suffix)
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '-' && i + 1 < len && std::isdigit(static_cast<unsigned char>(line[i + 1])))) {
            int start = i;
            if (c == '-') ++i;
            while (i < len && (std::isdigit(static_cast<unsigned char>(line[i])) || line[i] == '.')) ++i;
            // Consume 's' suffix (e.g., "0.15s")
            if (i < len && (line[i] == 's' || line[i] == '%')) ++i;
            out.push_back({start, i, TokenType::Number});
            continue;
        }

        // Hex color #RRGGBB inside value context (after colon)
        // Already covered by selector detection above when at line start.
        // Inside values, it starts with # followed by hex digits.

        // Identifier (property name or value keyword)
        if (is_ident_start(c) || c == '-') {
            int start = i;
            while (i < len && (is_ident_char(line[i]) || line[i] == '-')) ++i;
            std::string word(line.substr(start, i - start));

            if (fjss_properties().contains(word)) {
                out.push_back({start, i, TokenType::Attribute});
            } else if (fjss_values().contains(word)) {
                out.push_back({start, i, TokenType::Keyword});
            } else {
                out.push_back({start, i, TokenType::Default});
            }
            continue;
        }

        // Punctuation: { } ; :
        out.push_back({i, i + 1, TokenType::Punctuation});
        ++i;
    }

    return next;
}

// ---------------------------------------------------------------------------
// JSON tokenizer (simple)
// ---------------------------------------------------------------------------

static LineState tokenize_json(
    std::string_view line, LineState /*prev*/, std::vector<Token>& out)
{
    int i = 0;
    int len = static_cast<int>(line.size());

    while (i < len) {
        char c = line[i];

        if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }

        // String (keys and values)
        if (c == '"') {
            int start = i;
            ++i;
            while (i < len) {
                if (line[i] == '\\' && i + 1 < len) { i += 2; continue; }
                if (line[i] == '"') { ++i; break; }
                ++i;
            }
            out.push_back({start, i, TokenType::String});
            continue;
        }

        // Number
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '-') {
            int start = i;
            if (c == '-') ++i;
            while (i < len && (std::isdigit(static_cast<unsigned char>(line[i])) ||
                               line[i] == '.' || line[i] == 'e' || line[i] == 'E' ||
                               line[i] == '+' || line[i] == '-')) ++i;
            out.push_back({start, i, TokenType::Number});
            continue;
        }

        // Keywords: true, false, null
        if (is_ident_start(c)) {
            int start = i;
            while (i < len && is_ident_char(line[i])) ++i;
            std::string word(line.substr(start, i - start));
            if (word == "true" || word == "false" || word == "null") {
                out.push_back({start, i, TokenType::Keyword});
            } else {
                out.push_back({start, i, TokenType::Default});
            }
            continue;
        }

        out.push_back({i, i + 1, TokenType::Punctuation});
        ++i;
    }

    return {};
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

LineState tokenize_line(
    std::string_view line, Language lang, LineState prev,
    std::vector<Token>& out_tokens)
{
    out_tokens.clear();

    if (line.empty()) return prev.in_block_comment ? LineState{true} : LineState{};

    switch (lang) {
        case Language::Cpp: {
            CFamilyConfig cfg{&cpp_keywords(), &cpp_types(), nullptr, true};
            return tokenize_c_family(line, prev, out_tokens, cfg);
        }
        case Language::Glsl: {
            CFamilyConfig cfg{&glsl_keywords(), &glsl_types(), &glsl_builtins(), false};
            return tokenize_c_family(line, prev, out_tokens, cfg);
        }
        case Language::Fjsl: {
            // Merge GLSL + FJSL keyword/builtin sets
            static std::unordered_set<std::string> merged_kw;
            static std::unordered_set<std::string> merged_bi;
            static bool init = false;
            if (!init) {
                merged_kw = glsl_keywords();
                merged_kw.insert(fjsl_keywords().begin(), fjsl_keywords().end());
                merged_bi = glsl_builtins();
                merged_bi.insert(fjsl_builtins().begin(), fjsl_builtins().end());
                init = true;
            }
            CFamilyConfig cfg{&merged_kw, &glsl_types(), &merged_bi, false};
            return tokenize_c_family(line, prev, out_tokens, cfg);
        }
        case Language::Fjui:
            return tokenize_fjui(line, prev, out_tokens);
        case Language::Fjss:
            return tokenize_fjss(line, prev, out_tokens);
        case Language::Json:
            return tokenize_json(line, prev, out_tokens);
        case Language::PlainText:
            if (!line.empty()) {
                out_tokens.push_back({0, static_cast<int>(line.size()), TokenType::Default});
            }
            return {};
    }

    return {};
}

} // namespace fjell
