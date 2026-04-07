#include "parser.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

namespace fjreflect {

// ── Type mapping ────────────────────────────────────────────────────────

static std::string map_field_type(const std::string& cpp_type,
                                   std::string& ref_target) {
    ref_target.clear();

    // Strip leading namespaces for matching
    auto bare = cpp_type;
    if (bare.starts_with("fjell::")) bare = bare.substr(7);

    if (bare == "float") return "FJELL_FIELD_FLOAT";
    if (bare == "int" || bare == "int32_t") return "FJELL_FIELD_INT";
    if (bare == "bool") return "FJELL_FIELD_BOOL";
    if (bare == "glm::vec2") return "FJELL_FIELD_VEC2";
    if (bare == "glm::vec3") return "FJELL_FIELD_VEC3";
    if (bare == "glm::vec4") return "FJELL_FIELD_VEC4";
    if (bare == "std::string") return "FJELL_FIELD_STRING";

    // WeakRef<T>
    if (bare.starts_with("WeakRef<")) {
        auto start = bare.find('<') + 1;
        auto end = bare.rfind('>');
        ref_target = bare.substr(start, end - start);
        if (auto pos = ref_target.rfind("::"); pos != std::string::npos)
            ref_target = ref_target.substr(pos + 2);
        return "FJELL_FIELD_WEAK_REF";
    }

    // Ref<T>
    if (bare.starts_with("Ref<")) {
        auto start = bare.find('<') + 1;
        auto end = bare.rfind('>');
        ref_target = bare.substr(start, end - start);
        if (auto pos = ref_target.rfind("::"); pos != std::string::npos)
            ref_target = ref_target.substr(pos + 2);
        return "FJELL_FIELD_REF";
    }

    // SharedRef<T>
    if (bare.starts_with("SharedRef<")) {
        auto start = bare.find('<') + 1;
        auto end = bare.rfind('>');
        ref_target = bare.substr(start, end - start);
        if (auto pos = ref_target.rfind("::"); pos != std::string::npos)
            ref_target = ref_target.substr(pos + 2);
        return "FJELL_FIELD_SHARED_REF";
    }

    return {}; // Unknown
}

static bool is_engine_pointer(const std::string& type) {
    static const char* engine_types[] = {
        "Node3D", "Node", "SceneNode", "Component", "ScriptComponent",
        "Material", "Model", "Texture", "AudioClip", "Skeleton", "AnimationClip",
    };
    if (!type.ends_with("*")) return false;
    for (const auto* et : engine_types) {
        if (type.find(et) != std::string::npos) return true;
    }
    return false;
}

// ── Text-based parsing ──────────────────────────────────────────────────

static std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

/// Extract attribute content from [[fj::...]] patterns on a line.
/// Returns the full attribute string like "fj::property, fj::range(0, 100)"
static std::string extract_fj_attrs(const std::string& line) {
    // Find [[...]] that contains "fj::"
    auto open = line.find("[[");
    if (open == std::string::npos) return {};
    auto close = line.find("]]", open);
    if (close == std::string::npos) return {};

    auto content = line.substr(open + 2, close - open - 2);
    if (content.find("fj::") == std::string::npos) return {};
    return content;
}

/// Parse range hint from "fj::range(min, max)" inside attribute string.
static bool parse_range(const std::string& attrs, float& min, float& max) {
    std::regex re(R"(fj::range\(\s*([0-9.eE+-]+)\s*,\s*([0-9.eE+-]+)\s*\))");
    std::smatch m;
    if (std::regex_search(attrs, m, re)) {
        try {
            min = std::stof(m[1]);
            max = std::stof(m[2]);
            return true;
        } catch (...) {}
    }
    return false;
}

/// Parse a field declaration after stripping attributes.
/// Handles: "float speed = 10.0f;" and "WeakRef<Node3D> target;"
static bool parse_field_decl(const std::string& line, std::string& type, std::string& name) {
    // Remove everything from [[...]] to ]]
    auto clean = line;
    while (true) {
        auto open = clean.find("[[");
        if (open == std::string::npos) break;
        auto close = clean.find("]]", open);
        if (close == std::string::npos) break;
        clean.erase(open, close - open + 2);
    }
    clean = trim(clean);

    // Remove brace-initialized default value: "name{...}" -> "name"
    auto brace = clean.find('{');
    if (brace != std::string::npos) {
        clean = trim(clean.substr(0, brace));
    }

    // Remove default value (everything from " = " or "=" to ";")
    auto eq = clean.find('=');
    if (eq != std::string::npos) {
        clean = trim(clean.substr(0, eq));
    }

    // Remove trailing semicolon
    if (clean.ends_with(";")) clean.pop_back();
    clean = trim(clean);

    if (clean.empty()) return false;

    // Split into type and name: last token is name, everything before is type
    // Handle template types like WeakRef<Node3D>
    auto last_space = clean.rfind(' ');
    auto last_gt = clean.rfind('>');

    // If the last > is after the last space, we need to find the space before the template
    if (last_gt != std::string::npos && (last_space == std::string::npos || last_gt > last_space)) {
        // Find space after the closing >
        last_space = clean.find(' ', last_gt);
    }

    if (last_space == std::string::npos) return false;

    type = trim(clean.substr(0, last_space));
    name = trim(clean.substr(last_space + 1));

    return !type.empty() && !name.empty();
}

ParseResult parse_file(const std::string& filepath) {
    ParseResult result;

    std::ifstream file(filepath);
    if (!file) {
        result.errors.push_back(filepath + ": cannot open file");
        return result;
    }

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
        lines.push_back(line);
    }

    // Find FJELL_COMPONENT, FJELL_UI_WIDGET, FJELL_UI_COMPONENT, or FJELL_NOTIFY
    std::regex comp_re(R"(FJELL_COMPONENT\(\s*(\w+)\s*\))");
    std::regex ui_widget_re(R"(FJELL_UI_WIDGET\(\s*(\w+)\s*\))");
    std::regex ui_comp_re(R"RE(FJELL_UI_COMPONENT\(\s*(\w+)\s*,\s*"([^"]+)"\s*\))RE");
    std::regex notify_re(R"(FJELL_NOTIFY\(\s*(\w+)\s*\))");

    for (size_t i = 0; i < lines.size(); ++i) {
        std::smatch m;
        bool is_ui = false;
        bool is_notify = false;
        if (std::regex_search(lines[i], m, ui_widget_re)) {
            is_ui = true;
        } else if (std::regex_search(lines[i], m, ui_comp_re)) {
            is_ui = true;
        } else if (std::regex_search(lines[i], m, notify_re)) {
            is_notify = true;
        } else if (!std::regex_search(lines[i], m, comp_re)) {
            continue;
        }

        ComponentInfo comp;
        comp.class_name = m[1];
        comp.source_file = filepath;
        comp.is_ui_component = is_ui;
        comp.is_notify = is_notify;

        // Detect enclosing namespace by scanning backwards
        std::string ns;
        std::regex ns_re(R"(namespace\s+(\w+))");
        int ns_brace_depth = 0;
        for (size_t j = i; j-- > 0;) {
            for (auto ch : lines[j]) {
                if (ch == '}') ns_brace_depth++;
                if (ch == '{') ns_brace_depth--;
            }
            std::smatch nm;
            if (ns_brace_depth < 0 && std::regex_search(lines[j], nm, ns_re)) {
                ns = nm[1].str();
                break;
            }
        }
        comp.qualified_name = ns.empty() ? comp.class_name : ns + "::" + comp.class_name;

        // Scan forward for fields until we hit closing brace or end of file
        // Track brace depth to know when the class ends
        int brace_depth = 0;
        bool in_class = false;

        // Find the opening brace of this class (might be before FJELL_COMPONENT)
        for (size_t j = i; j-- > 0;) {
            if (lines[j].find('{') != std::string::npos) {
                brace_depth = 1;
                in_class = true;
                break;
            }
            if (lines[j].find("class ") != std::string::npos ||
                lines[j].find("struct ") != std::string::npos) {
                break;
            }
        }
        if (!in_class) {
            // Opening brace might be on the same line or after
            for (size_t j = i; j < lines.size(); ++j) {
                if (lines[j].find('{') != std::string::npos) {
                    brace_depth = 1;
                    in_class = true;
                    break;
                }
            }
        }

        // Now scan from after FJELL_COMPONENT line for fields
        std::string pending_attrs;
        unsigned pending_line = 0;

        for (size_t j = i + 1; j < lines.size(); ++j) {
            auto& ln = lines[j];

            // Track brace depth
            for (char c : ln) {
                if (c == '{') brace_depth++;
                else if (c == '}') brace_depth--;
            }
            if (brace_depth <= 0) break; // End of class

            auto trimmed = trim(ln);

            // Check for [[fj::action]] on void methods (UI widgets only for codegen)
            auto attrs_early = extract_fj_attrs(ln);
            if (!attrs_early.empty() && attrs_early.find("fj::action") != std::string::npos
                && comp.is_ui_component) {
                // Parse action: [[fj::action]] void method_name(int param) { ... }
                // or [[fj::action]] on its own line, method on next
                auto action_line = trimmed;
                // Strip [[...]]
                auto bracket_start = action_line.find("[[");
                auto bracket_end = action_line.find("]]");
                if (bracket_start != std::string::npos && bracket_end != std::string::npos) {
                    action_line = trim(action_line.substr(bracket_end + 2));
                }
                if (action_line.empty() && j + 1 < lines.size()) {
                    // Attribute on its own line, method on next
                    ++j;
                    action_line = trim(lines[j]);
                    for (char c : lines[j]) {
                        if (c == '{') brace_depth++;
                        else if (c == '}') brace_depth--;
                    }
                }
                // Parse "void method_name(...)"
                if (action_line.starts_with("void ")) {
                    auto rest = action_line.substr(5);
                    auto paren = rest.find('(');
                    if (paren != std::string::npos) {
                        ActionInfo ai;
                        ai.method_name = trim(rest.substr(0, paren));
                        ai.name = ai.method_name;  // default: action name = method name
                        // Parse trigger mode from [[fj::action(...)]]
                        // Supports: "pressed", pressed, FjellTrigger::Pressed, Pressed
                        auto action_arg_pos = attrs_early.find("fj::action(");
                        if (action_arg_pos != std::string::npos) {
                            auto arg_start = action_arg_pos + 11; // after "fj::action("
                            auto arg_end = attrs_early.find(')', arg_start);
                            if (arg_end != std::string::npos) {
                                auto arg = trim(attrs_early.substr(arg_start, arg_end - arg_start));
                                // Strip quotes if present
                                if (arg.size() >= 2 && arg.front() == '"' && arg.back() == '"') {
                                    arg = arg.substr(1, arg.size() - 2);
                                }
                                // Strip enum prefix (FjellTrigger:: or any Foo::)
                                auto colon_pos = arg.rfind("::");
                                if (colon_pos != std::string::npos) {
                                    arg = arg.substr(colon_pos + 2);
                                }
                                // Case-insensitive compare
                                for (auto& c : arg) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                                if (arg == "pressed") ai.trigger = ActionTriggerMode::pressed;
                                else if (arg == "released") ai.trigger = ActionTriggerMode::released;
                                else if (arg == "completed") ai.trigger = ActionTriggerMode::completed;
                                else if (arg == "triggered") ai.trigger = ActionTriggerMode::triggered;
                            }
                        }
                        // Detect parameter type
                        auto params = rest.substr(paren + 1);
                        auto close_paren = params.find(')');
                        if (close_paren != std::string::npos) {
                            auto param_str = trim(params.substr(0, close_paren));
                            if (param_str == "int" || param_str.starts_with("int ") ||
                                param_str.find("int ") != std::string::npos) {
                                ai.param = ActionParamType::int_param;
                                ai.has_int_param = true;
                            } else if (param_str == "float" || param_str.starts_with("float ")) {
                                ai.param = ActionParamType::float_param;
                            } else if (param_str.find("vec2") != std::string::npos) {
                                ai.param = ActionParamType::vec2_param;
                            }
                        }
                        comp.actions.push_back(std::move(ai));
                    }
                }
                pending_attrs.clear();
                continue;
            }

            // Skip access specifiers, methods, nested structs, etc.
            if (trimmed.starts_with("public:") || trimmed.starts_with("private:") ||
                trimmed.starts_with("protected:") || trimmed.starts_with("//") ||
                trimmed.starts_with("static ") || trimmed.starts_with("virtual ") ||
                trimmed.starts_with("void ") || trimmed.starts_with("auto ") ||
                trimmed.starts_with("struct ") || trimmed.starts_with("class ") ||
                trimmed.starts_with("friend ") || trimmed.starts_with("using ") ||
                trimmed.starts_with("template") || trimmed.starts_with("#") ||
                trimmed.empty()) {
                pending_attrs.clear();
                continue;
            }

            // Check for [[fj::...]] attributes
            auto attrs = extract_fj_attrs(ln);

            bool is_property = attrs.find("fj::property") != std::string::npos;
            bool is_bind = attrs.find("fj::bind_list") != std::string::npos ? false
                         : attrs.find("fj::bind") != std::string::npos;
            bool is_bind_list = attrs.find("fj::bind_list") != std::string::npos;

            // fj::bind and fj::bind_list imply fj::property
            if (is_bind || is_bind_list) is_property = true;

            if (!attrs.empty() && is_property) {
                // Attribute and field might be on same line or attribute on its own line
                std::string type_str, name_str;
                if (parse_field_decl(ln, type_str, name_str)) {
                    // Same line: [[fj::property]] float speed = 10.0f;
                    pending_attrs = attrs;
                    pending_line = static_cast<unsigned>(j + 1);
                } else {
                    // Attribute on its own line, field on next
                    pending_attrs = attrs;
                    pending_line = static_cast<unsigned>(j + 1);
                    continue;
                }

                // Process the field
                // [[fj::bind]] on a non-engine pointer → widget pointer binding
                if (is_bind && type_str.ends_with("*") && !is_engine_pointer(type_str)) {
                    FieldInfo fi;
                    fi.name = name_str;
                    fi.cpp_type = type_str;
                    fi.fj_field_type = "FJELL_FIELD_WIDGET_PTR";
                    // Extract pointed-to type name (strip trailing * and namespace)
                    auto bare = type_str.substr(0, type_str.size() - 1);
                    while (!bare.empty() && bare.back() == ' ') bare.pop_back();
                    if (auto ns_pos = bare.rfind("::"); ns_pos != std::string::npos)
                        bare = bare.substr(ns_pos + 2);
                    fi.ref_target = bare;
                    fi.line = pending_line;
                    fi.is_bind = true;
                    comp.fields.push_back(std::move(fi));
                    pending_attrs.clear();
                    continue;
                }

                if (is_engine_pointer(type_str)) {
                    result.errors.push_back(
                        filepath + ":" + std::to_string(j + 1) +
                        ": error: [[fj::property]] on raw pointer '" + type_str +
                        " " + name_str + "' — use WeakRef<T> or Ref<T>");
                    pending_attrs.clear();
                    continue;
                }

                std::string ref_target;
                auto fj_type = map_field_type(type_str, ref_target);
                if (fj_type.empty()) {
                    result.errors.push_back(
                        filepath + ":" + std::to_string(j + 1) +
                        ": error: unsupported type '" + type_str +
                        "' for [[fj::property]] on '" + name_str + "'");
                    pending_attrs.clear();
                    continue;
                }

                FieldInfo fi;
                fi.name = name_str;
                fi.cpp_type = type_str;
                fi.fj_field_type = fj_type;
                fi.ref_target = ref_target;
                fi.line = pending_line;
                fi.is_bind = is_bind;
                fi.is_bind_list = is_bind_list;
                if (parse_range(pending_attrs, fi.hint_min, fi.hint_max)) {
                    fi.has_range = true;
                }

                comp.fields.push_back(std::move(fi));
                pending_attrs.clear();
                continue;
            }

            // Check if this line is a field following a pending attribute
            if (!pending_attrs.empty()) {
                bool p_is_bind = pending_attrs.find("fj::bind_list") != std::string::npos ? false
                               : pending_attrs.find("fj::bind") != std::string::npos;
                bool p_is_bind_list = pending_attrs.find("fj::bind_list") != std::string::npos;

                std::string type_str, name_str;
                if (parse_field_decl(ln, type_str, name_str)) {
                    // [[fj::bind]] on a non-engine pointer → widget pointer binding
                    if (p_is_bind && type_str.ends_with("*") && !is_engine_pointer(type_str)) {
                        FieldInfo fi;
                        fi.name = name_str;
                        fi.cpp_type = type_str;
                        fi.fj_field_type = "FJELL_FIELD_WIDGET_PTR";
                        auto bare = type_str.substr(0, type_str.size() - 1);
                        while (!bare.empty() && bare.back() == ' ') bare.pop_back();
                        if (auto ns_pos = bare.rfind("::"); ns_pos != std::string::npos)
                            bare = bare.substr(ns_pos + 2);
                        fi.ref_target = bare;
                        fi.line = static_cast<unsigned>(j + 1);
                        fi.is_bind = true;
                        comp.fields.push_back(std::move(fi));
                    } else if (is_engine_pointer(type_str)) {
                        result.errors.push_back(
                            filepath + ":" + std::to_string(j + 1) +
                            ": error: [[fj::property]] on raw pointer '" + type_str +
                            " " + name_str + "' — use WeakRef<T> or Ref<T>");
                    } else {
                        std::string ref_target;
                        auto fj_type = map_field_type(type_str, ref_target);
                        if (fj_type.empty()) {
                            result.errors.push_back(
                                filepath + ":" + std::to_string(j + 1) +
                                ": error: unsupported type '" + type_str +
                                "' for [[fj::property]] on '" + name_str + "'");
                        } else {
                            FieldInfo fi;
                            fi.name = name_str;
                            fi.cpp_type = type_str;
                            fi.fj_field_type = fj_type;
                            fi.ref_target = ref_target;
                            fi.line = static_cast<unsigned>(j + 1);
                            fi.is_bind = p_is_bind;
                            fi.is_bind_list = p_is_bind_list;
                            if (parse_range(pending_attrs, fi.hint_min, fi.hint_max)) {
                                fi.has_range = true;
                            }
                            comp.fields.push_back(std::move(fi));
                        }
                    }
                }
                pending_attrs.clear();
                continue;
            }

            // Check for raw pointers without [[fj::property]] (warning)
            std::string type_str, name_str;
            if (parse_field_decl(ln, type_str, name_str) && is_engine_pointer(type_str)) {
                result.warnings.push_back(
                    filepath + ":" + std::to_string(j + 1) +
                    ": warning: untracked raw pointer '" + type_str +
                    " " + name_str + "' to engine type");
            }
        }

        if (!comp.fields.empty() || !comp.actions.empty() || comp.is_ui_component) {
            result.components.push_back(std::move(comp));
        }
    }

    return result;
}

// ── Offset query via compiler ───────────────────────────────────────────

bool query_offsets(std::vector<ComponentInfo>& components,
                   const std::string& source_file,
                   const std::vector<std::string>& include_dirs,
                   const std::string& compiler) {
    if (components.empty()) return true;

    // Generate a temporary C++ file that prints offsets
    auto tmp_dir = std::filesystem::temp_directory_path();
    auto query_src = tmp_dir / "fjreflect_query.cpp";
    auto query_bin = tmp_dir / "fjreflect_query";

    {
        std::ofstream out(query_src);
        if (!out) {
            std::cerr << "fjreflect: cannot write " << query_src << "\n";
            return false;
        }

        // Stub engine extern "C" symbols that registration macros reference.
        // Headers only declare classes + fields, no engine method bodies.
        out << "struct FjellTypeRegistry;\n";
        out << "struct FjellComponentTypeOOP;\n";
        out << "struct FjellUiWidgetTypeOOP;\n";
        out << "using FjellUiComponentTypeOOP = FjellUiWidgetTypeOOP;\n";
        out << "struct FjellGeneratedInfo;\n";
        out << "struct FjellUiGeneratedInfo;\n";
        out << "struct FjellNotifyTypeOOP;\n";
        out << "extern \"C\" void fjell_reg_component(FjellTypeRegistry*, const FjellComponentTypeOOP*) {}\n";
        out << "extern \"C\" void fjell_reg_ui_component(FjellTypeRegistry*, const FjellUiWidgetTypeOOP*) {}\n";
        out << "extern \"C\" void fjell_reg_notify(FjellTypeRegistry*, const FjellNotifyTypeOOP*) {}\n";
        out << "extern \"C\" void fjell_set_generated_table(const FjellGeneratedInfo*, unsigned) {}\n";
        out << "extern \"C\" void fjell_set_ui_generated_table(const FjellUiGeneratedInfo*, unsigned) {}\n";
        out << "extern \"C\" const FjellUiGeneratedInfo* fjell_find_ui_generated(const char*) { return nullptr; }\n";
        out << "extern \"C\" const FjellGeneratedInfo* fjell_find_generated(const char*) { return nullptr; }\n";
        out << "extern \"C\" void* fjell_resolve_weak_ref(unsigned, unsigned) { return nullptr; }\n";
        out << "extern \"C\" void fjell_ref_acquire(unsigned) {}\n";
        out << "extern \"C\" void fjell_ref_release(unsigned) {}\n";
        out << "extern \"C\" void* fjell_ref_resolve(unsigned) { return nullptr; }\n";
        out << "extern \"C\" void fjell_shared_ref_acquire(unsigned) {}\n";
        out << "extern \"C\" void fjell_shared_ref_release(unsigned) {}\n";
        out << "extern \"C\" void* fjell_shared_ref_resolve(unsigned) { return nullptr; }\n";
        out << "extern \"C\" void fjell_gc_trace_weak(void*, void*) {}\n";
        out << "extern \"C\" void fjell_gc_trace_ref(void*, void*) {}\n";
        out << "extern \"C\" void fjell_gc_trace_shared(void*, void*) {}\n\n";

        out << "#include \"" << std::filesystem::path(source_file).filename().string() << "\"\n";
        out << "#include <cstdio>\n";
        out << "#include <cstddef>\n";
        out << "int main() {\n";

        for (const auto& comp : components) {
            for (const auto& f : comp.fields) {
                out << "    printf(\"" << comp.class_name << " " << f.name
                    << " %zu %zu\\n\", offsetof(" << comp.qualified_name << ", " << f.name
                    << "), sizeof(((" << comp.qualified_name << "*)nullptr)->" << f.name << "));\n";
            }
        }

        out << "    return 0;\n}\n";
    }

    // Build compiler command
    bool is_msvc = compiler.find("cl") != std::string::npos
                && compiler.find("clang") == std::string::npos;

    std::string includes;
    auto source_dir = std::filesystem::path(source_file).parent_path().generic_string();
    if (!source_dir.empty()) {
        if (is_msvc)
            includes += " /I\"" + source_dir + "\"";
        else
            includes += " -I" + source_dir;
    }
    for (const auto& dir : include_dirs) {
        if (is_msvc)
            includes += " /I\"" + dir + "\"";
        else
            includes += " -I" + dir;
    }

    std::string cmd;
    if (is_msvc) {
        auto query_exe = query_bin;
        query_exe += ".exe";
        cmd = "\"" + compiler + "\" /std:c++latest /w /EHsc"
            + includes + " /Fe\"" + query_exe.string() + "\""
            + " \"" + query_src.string() + "\""
            + " /link /NOLOGO 2>&1";
        std::cerr << "fjreflect: MSVC cmd: " << cmd << "\n";
        query_bin = query_exe;
    } else {
        cmd = compiler + " -std=c++23 -Wno-attributes -Wno-invalid-offsetof -w"
            " -Wl,--unresolved-symbols=ignore-in-object-files"
            + includes + " -o " + query_bin.string() + " " + query_src.string() + " 2>&1";
    }

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        std::cerr << "fjreflect: cannot run compiler\n";
        return false;
    }
    std::string compile_output;
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe)) {
        compile_output += buf;
    }
    int status = pclose(pipe);
    if (status != 0) {
        std::cerr << "fjreflect: offset query compilation failed:\n" << compile_output;
        std::filesystem::remove(query_src);
        return false;
    }

    // Run
    pipe = popen(query_bin.string().c_str(), "r");
    if (!pipe) {
        std::cerr << "fjreflect: cannot run offset query\n";
        return false;
    }

    // Parse output: "ClassName fieldname offset size"
    std::string line;
    while (fgets(buf, sizeof(buf), pipe)) {
        std::istringstream iss(buf);
        std::string class_name, field_name;
        size_t offset, size;
        if (iss >> class_name >> field_name >> offset >> size) {
            for (auto& comp : components) {
                if (comp.class_name != class_name) continue;
                for (auto& f : comp.fields) {
                    if (f.name == field_name) {
                        f.offset = static_cast<uint32_t>(offset);
                        f.size = static_cast<uint32_t>(size);
                    }
                }
            }
        }
    }
    pclose(pipe);

    // Cleanup
    std::filesystem::remove(query_src);
    std::filesystem::remove(query_bin);
    std::filesystem::remove(tmp_dir / "fjreflect_query.o");
    std::filesystem::remove(tmp_dir / "fjreflect_query.obj"); // MSVC object file

    return true;
}

} // namespace fjreflect
