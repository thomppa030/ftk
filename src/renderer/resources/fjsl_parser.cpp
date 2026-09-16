#include "renderer/resources/fjsl_parser.hpp"
#include "core/log.hpp"
#include "core/math/color_space.hpp"

#include <algorithm>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace fjell {

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
static const std::regex& uniform_declaration_re() {
    static const std::regex re(
        R"(uniform\s+(\w+)\s+(\w+)\s*(?::\s*([^=;]+?))?\s*(?:=\s*([^;]+))?\s*;)");
    return re;
}

static std::string trim(const std::string& str) {
    auto s = str.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) return {};
    auto e = str.find_last_not_of(" \t\r\n");
    return str.substr(s, e - s + 1);
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

static void apply_hint(FjslUniform& u, const std::string& token) {
    if (token.rfind("hint_range", 0) == 0) {
        // hint_range(min, max[, step]) — stored beside the UI hint rather
        // than as one, since a ranged float has no picker to choose.
        auto open = token.find('(');
        auto close = token.rfind(')');
        if (open == std::string::npos || close == std::string::npos || close < open) {
            FJELL_GFX_WARN("FJSL: hint_range on '{}' has no argument list", u.name);
            return;
        }
        std::istringstream as(token.substr(open + 1, close - open - 1));
        std::string tok;
        std::vector<float> vals;
        while (std::getline(as, tok, ',')) {
            auto t = trim(tok);
            if (!t.empty()) vals.push_back(std::stof(t));
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
        FJELL_GFX_WARN("FJSL: unknown hint '{}' on uniform '{}'", token, u.name);
        return;
    }
    if (u.hint != UniformHint::None && u.hint != hint) {
        FJELL_GFX_WARN("FJSL: uniform '{}' declares more than one presentation hint; "
                       "'{}' wins", u.name, token);
    }
    u.hint = hint;
}

static std::vector<float> parse_default_value(const std::string& val_str, UniformType type) {
    std::vector<float> result;
    // Match patterns like "0.5", "vec3(1.0, 0.5, 0.2)", "vec4(1.0)"
    std::string trimmed = val_str;

    // Strip vec2/vec3/vec4 wrapper
    static const std::regex vec_re(R"(vec[234]\s*\((.*)\))");
    std::smatch vm;
    if (std::regex_match(trimmed, vm, vec_re)) {
        trimmed = vm[1].str();
    }

    // Split by commas
    std::istringstream ss(trimmed);
    std::string token;
    while (std::getline(ss, token, ',')) {
        // Trim whitespace
        auto start = token.find_first_not_of(" \t");
        auto end = token.find_last_not_of(" \t");
        if (start != std::string::npos) {
            result.push_back(std::stof(token.substr(start, end - start + 1)));
        }
    }

    // If vec3(1.0) given, expand single value
    uint32_t expected = uniform_float_count(type);
    if (result.size() == 1 && expected > 1) {
        float v = result[0];
        result.resize(expected, v);
    }

    return result;
}

static std::string extract_function_body(const std::string& source, const std::string& func_name) {
    auto pos = source.find("void " + func_name);
    if (pos == std::string::npos) return "";

    // Find opening brace
    auto brace_start = source.find('{', pos);
    if (brace_start == std::string::npos) return "";

    // Match braces
    int depth = 1;
    size_t i = brace_start + 1;
    while (i < source.size() && depth > 0) {
        if (source[i] == '{') depth++;
        else if (source[i] == '}') depth--;
        i++;
    }

    if (depth != 0) return "";
    return source.substr(brace_start + 1, i - brace_start - 2);
}

// Remove the entire `<sig> { ... }` block for a given top-level function.
// Used to strip fragment/vertex/light from the source so what's left
// becomes user-authored file-scope code (helpers, constants).
static void strip_function(std::string& source, const std::string& func_name) {
    // Match an identifier-boundary occurrence to avoid stripping
    // "fragment_helper" when looking for "fragment".
    size_t search_from = 0;
    while (search_from < source.size()) {
        auto pos = source.find(func_name, search_from);
        if (pos == std::string::npos) return;
        // Check left boundary (start of source or non-identifier char before)
        bool left_ok = (pos == 0) || !(std::isalnum(static_cast<unsigned char>(source[pos - 1])) || source[pos - 1] == '_');
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
        if (brace_start == std::string::npos) return;
        int depth = 1;
        size_t i = brace_start + 1;
        while (i < source.size() && depth > 0) {
            if (source[i] == '{') depth++;
            else if (source[i] == '}') depth--;
            i++;
        }
        if (depth != 0) return;
        source.erase(sig_start, i - sig_start);
        search_from = sig_start;
    }
}

// Return user-written file-scope code: everything except shader_type,
// render_mode, uniform declarations, and the three reserved functions
// (fragment/vertex/light). Lets users author helper functions, constants,
// and structs that the spatial template injects at file scope.
static std::string extract_user_globals(const std::string& source) {
    std::string s = source;
    // Strip shader_type / render_mode lines.
    static const std::regex shader_type_re(R"(shader_type\s+\w+\s*;)");
    static const std::regex render_mode_re(R"(render_mode\s+[^;]+;)");
    s = std::regex_replace(s, shader_type_re, std::string{});
    s = std::regex_replace(s, render_mode_re, std::string{});
    // Strip uniform declarations.
    s = std::regex_replace(s, uniform_declaration_re(), std::string{});
    // Strip the reserved function definitions (fragment/vertex/light for
    // spatial shaders; sky for shader_type sky; layers for terrain).
    // Whatever's left is the user's file-scope code — helpers, structs,
    // constants.
    strip_function(s, "fragment");
    strip_function(s, "vertex");
    strip_function(s, "light");
    strip_function(s, "sky");
    strip_function(s, "layers");
    return s;
}

ShaderMetadata parse_fjsl(const std::string& source) {
    ShaderMetadata meta;

    // Parse shader_type
    static const std::regex shader_type_re(R"(shader_type\s+(\w+)\s*;)");
    std::smatch sm;
    if (std::regex_search(source, sm, shader_type_re)) {
        meta.shader_type = sm[1].str();
    } else {
        throw std::runtime_error("FJSL: missing shader_type declaration");
    }

    if (meta.shader_type != "spatial" && meta.shader_type != "sky" &&
        meta.shader_type != "ui" && meta.shader_type != "terrain" &&
        meta.shader_type != "water") {
        throw std::runtime_error("FJSL: unsupported shader_type '" + meta.shader_type + "'");
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
            else FJELL_GFX_WARN("FJSL: unknown render_mode '{}'", mode);
        }
    }

    // Parse uniform declarations
    auto begin = std::sregex_iterator(source.begin(), source.end(), uniform_declaration_re());
    auto end = std::sregex_iterator();

    uint32_t texture_slot = 4; // custom textures start at slot 4
    uint32_t param_offset = 0;

    for (auto it = begin; it != end; ++it) {
        FjslUniform u;
        u.type = parse_type((*it)[1].str());
        u.name = (*it)[2].str();

        if ((*it)[3].matched) {
            for (const auto& token : split_hint_list((*it)[3].str())) {
                apply_hint(u, token);
            }
        }

        if ((*it)[4].matched) {
            std::string def_str = (*it)[4].str();
            // Trim whitespace
            auto s = def_str.find_first_not_of(" \t");
            auto e = def_str.find_last_not_of(" \t");
            if (s != std::string::npos) {
                def_str = def_str.substr(s, e - s + 1);
            }
            u.default_value = parse_default_value(def_str, u.type);
        }

        if (u.type == UniformType::Sampler2D) {
            if (texture_slot > 7) {
                FJELL_GFX_WARN("FJSL: too many custom sampler2D uniforms (max 4), skipping '{}'", u.name);
                continue;
            }
            u.slot = texture_slot++;
            meta.custom_texture_count++;
        } else {
            u.slot = param_offset;
            uint32_t count = uniform_float_count(u.type);
            param_offset += count;
        }

        meta.uniforms.push_back(u);
    }

    meta.custom_param_count = param_offset;
    // Sky shaders supply their user code via sky() rather than fragment();
    // route it through the same fragment_body slot so the existing
    // /*FJSL_USER_CODE*/ substitution path works unchanged.
    meta.fragment_body = meta.shader_type == "sky"
        ? extract_function_body(source, "sky")
        : extract_function_body(source, "fragment");
    meta.vertex_body = extract_function_body(source, "vertex");
    meta.light_body = extract_function_body(source, "light");
    meta.layers_body = extract_function_body(source, "layers");
    meta.user_globals = extract_user_globals(source);

    // A hook that a shader type cannot run is an error rather than silently
    // ignored code. A ui shader paints one screen-aligned quad: there is no
    // geometry to displace and no light to shade. Terrain geometry is
    // generated in a mesh shader from the height texture, and water's from
    // its wave field, so neither has anything for vertex() to move — a water
    // surface is shaped by its .fjwater, not by its material. layers()
    // decides how the painted layer weights blend and only terrain has
    // layers.
    static const std::regex vertex_hook_re(R"(\bvoid\s+vertex\s*\()");
    static const std::regex light_hook_re(R"(\bvoid\s+light\s*\()");
    static const std::regex layers_hook_re(R"(\bvoid\s+layers\s*\()");
    if (meta.shader_type == "ui") {
        if (std::regex_search(source, vertex_hook_re)) {
            throw std::runtime_error("FJSL: shader_type ui has no vertex() stage");
        }
        if (std::regex_search(source, light_hook_re)) {
            throw std::runtime_error("FJSL: shader_type ui has no light() stage");
        }
    }
    if (meta.shader_type == "terrain" && std::regex_search(source, vertex_hook_re)) {
        throw std::runtime_error("FJSL: shader_type terrain has no vertex() stage");
    }
    if (meta.shader_type == "water" && std::regex_search(source, vertex_hook_re)) {
        throw std::runtime_error("FJSL: shader_type water has no vertex() stage — "
                                 "the surface comes from the .fjwater wave field");
    }
    if (meta.shader_type != "terrain" && std::regex_search(source, layers_hook_re)) {
        throw std::runtime_error("FJSL: only shader_type terrain has a layers() stage");
    }

    return meta;
}

std::string generate_fragment_shader(const ShaderMetadata& meta,
                                     const std::string& template_source) {
    std::string result = template_source;

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
    const std::string mat_expr =
        meta.shader_type == "sky" ? "materials[push.sky_material_id]"
        : meta.shader_type == "terrain" ? "materials[push.terrain_material_id]"
        : meta.shader_type == "water" ? "materials[push.water_material_id]"
        : meta.shader_type == "ui" ? "materials[quads[frag_quad_index].material_id]"
        : "materials[draws[frag_draw_index].material_id]";
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

    replace("/*FJSL_CUSTOM_TEXTURES*/", tex_defines + param_defines);
    // The CUSTOM_PARAMS placeholder originally injected local reads inside
    // main(); now param values come from the file-scope #defines emitted
    // above, so the in-main slot becomes empty.
    replace("/*FJSL_CUSTOM_PARAMS*/", std::string{});
    replace("/*FJSL_USER_GLOBALS*/", meta.user_globals);
    replace("/*FJSL_USER_CODE*/", meta.fragment_body);
    replace("/*FJSL_LAYERS_USER_CODE*/", meta.layers_body);

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
    replace("/*FJSL_LIGHT_USER_CODE*/",
            meta.light_body.empty() ? std::string(DEFAULT_LIGHT_BODY) : meta.light_body);

    // Replace PBR lighting block with direct output for unlit shaders
    if (meta.render_mode == RenderMode::Unlit) {
        const std::string lighting_marker = "    // \xe2\x94\x80\xe2\x94\x80 PBR lighting";
        auto lighting_pos = result.find(lighting_marker);
        if (lighting_pos != std::string::npos) {
            // Find the closing brace of main() — last '}' in the file
            auto main_end = result.rfind('}');
            if (main_end != std::string::npos) {
                result.replace(lighting_pos, main_end - lighting_pos,
                    "    out_color = vec4(ALBEDO + EMISSION, ALPHA);\n"
                    "    out_metallic_roughness = vec2(0.0);\n");
            }
        }
    }

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
    replace("/*FJSL_VERTEX_USER_CODE*/", meta.vertex_body);

    return result;
}

} // namespace fjell
