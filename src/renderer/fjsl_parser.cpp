#include "renderer/fjsl_parser.hpp"
#include "core/log.hpp"

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

    if (meta.shader_type != "spatial") {
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
            else if (mode == "cull_disabled") meta.render_state.cull_disabled = true;
            else if (mode == "cull_front") meta.render_state.cull_front = true;
            else if (mode == "depth_write_disabled") meta.render_state.depth_write_disabled = true;
            else if (mode == "depth_test_disabled") meta.render_state.depth_test_disabled = true;
            else if (mode == "blend_add") meta.render_state.blend = BlendMode::Add;
            else if (mode == "blend_multiply") meta.render_state.blend = BlendMode::Multiply;
            else FJELL_GFX_WARN("FJSL: unknown render_mode '{}'", mode);
        }
    }

    // Parse uniform declarations
    // uniform <type> <name> [: <hint>[(args)]] [= <default>];
    static const std::regex uniform_re(
        R"(uniform\s+(\w+)\s+(\w+)\s*(?::\s*(\w+)(?:\s*\(([^)]*)\))?)?\s*(?:=\s*([^;]+))?\s*;)");

    auto begin = std::sregex_iterator(source.begin(), source.end(), uniform_re);
    auto end = std::sregex_iterator();

    uint32_t texture_slot = 4; // custom textures start at slot 4
    uint32_t param_offset = 0;

    for (auto it = begin; it != end; ++it) {
        FjslUniform u;
        u.type = parse_type((*it)[1].str());
        u.name = (*it)[2].str();

        if ((*it)[3].matched) {
            std::string hint_str = (*it)[3].str();
            if (hint_str == "hint_range") {
                u.hint = UniformHint::None; // range is stored separately
                if ((*it)[4].matched) {
                    // Parse range args: min, max[, step]
                    std::string args = (*it)[4].str();
                    std::istringstream as(args);
                    std::string tok;
                    std::vector<float> vals;
                    while (std::getline(as, tok, ',')) {
                        auto s = tok.find_first_not_of(" \t");
                        auto e = tok.find_last_not_of(" \t");
                        if (s != std::string::npos) {
                            vals.push_back(std::stof(tok.substr(s, e - s + 1)));
                        }
                    }
                    if (vals.size() >= 2) {
                        u.range.min = vals[0];
                        u.range.max = vals[1];
                    }
                    if (vals.size() >= 3) {
                        u.range.step = vals[2];
                    }
                }
            } else {
                u.hint = parse_hint_name(hint_str);
            }
        }

        if ((*it)[5].matched) {
            std::string def_str = (*it)[5].str();
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
    meta.fragment_body = extract_function_body(source, "fragment");
    meta.vertex_body = extract_function_body(source, "vertex");

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

    // Generate custom param reads
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

    // Substitute placeholders
    auto replace = [&](const std::string& placeholder, const std::string& replacement) {
        auto pos = result.find(placeholder);
        if (pos != std::string::npos) {
            result.replace(pos, placeholder.size(), replacement);
        }
    };

    replace("/*FJSL_CUSTOM_TEXTURES*/", tex_defines);
    replace("/*FJSL_CUSTOM_PARAMS*/", param_reads);
    replace("/*FJSL_USER_CODE*/", meta.fragment_body);

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
