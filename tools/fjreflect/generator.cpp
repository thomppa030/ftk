#include "generator.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>

namespace fjreflect {

namespace {

/// The reference kinds among a component's fields, each traced for the
/// garbage collector through its own bridge function.
struct RefKinds {
    bool weak{false};
    bool ref{false};
    bool shared{false};
    [[nodiscard]] bool any() const { return weak || ref || shared; }
};

RefKinds ref_kinds(const ComponentInfo& comp) {
    RefKinds kinds;
    for (const auto& f : comp.fields) {
        if (f.fj_field_type == "FJELL_FIELD_WEAK_REF") kinds.weak = true;
        if (f.fj_field_type == "FJELL_FIELD_REF") kinds.ref = true;
        if (f.fj_field_type == "FJELL_FIELD_SHARED_REF") kinds.shared = true;
    }
    return kinds;
}

std::string stem_of(const ComponentInfo& comp) {
    return std::filesystem::path(comp.source_file).stem().string();
}

/// The friend every component macro declares, specialised for this class:
/// the offset and size of each reflected field, measured by the compiler
/// that builds the game with the game's own flags, and the shims through
/// which generated code reaches private members.
void emit_access(std::ostream& out, const ComponentInfo& comp) {
    if (comp.fields.empty() && comp.rpcs.empty()) return;
    const auto& q = comp.qualified_name;
    out << "namespace fjell { template <> struct FjellAccess<" << q << "> {\n";
    for (const auto& f : comp.fields) {
        out << "    static constexpr uint32_t offset_" << f.name << " = static_cast<uint32_t>(offsetof("
            << q << ", " << f.name << "));\n";
        out << "    static constexpr uint32_t size_" << f.name << " = static_cast<uint32_t>(sizeof("
            << q << "::" << f.name << "));\n";
    }
    // Handlers are private `_impl` methods more often than not.
    for (const auto& rpc : comp.rpcs) {
        out << "    static void " << rpc.method_name << "(" << q << "* self";
        for (size_t p = 0; p < rpc.params.size(); ++p) out << ", " << rpc.params[p].cpp_type << " a" << p;
        out << ") { self->" << rpc.method_name << "_impl(";
        for (size_t p = 0; p < rpc.params.size(); ++p) {
            out << "a" << p;
            if (p + 1 < rpc.params.size()) out << ", ";
        }
        out << "); }\n";
    }
    out << "}; }\n\n";
}

/// One component's field table, trace function, action trampolines and RPC
/// code, in that order.
void emit_component(std::ostream& out, const ComponentInfo& comp) {
    const auto kinds = ref_kinds(comp);
    const auto access = "fjell::FjellAccess<" + comp.qualified_name + ">::";

    emit_access(out, comp);

    // Static enum label arrays (one per ENUM field)
    out << std::fixed;
    auto label_symbol = [&](const std::string& field_name) {
        return "fjreflect_" + comp.class_name + "_" + field_name + "_labels";
    };
    for (const auto& f : comp.fields) {
        if (f.enum_labels.empty()) continue;
        out << "static const char* const " << label_symbol(f.name) << "[] = {";
        for (std::size_t e = 0; e < f.enum_labels.size(); ++e) {
            if (e) out << ", ";
            out << "\"" << f.enum_labels[e] << "\"";
        }
        out << "};\n";
    }
    if (std::any_of(comp.fields.begin(), comp.fields.end(),
                    [](const auto& f) { return !f.enum_labels.empty(); })) {
        out << "\n";
    }

    // Field table. A class without fields has none: a zero-length array is not
    // C++, and register_all.cpp passes nullptr for it.
    if (!comp.fields.empty()) out << "extern \"C\" const FjellFieldDesc fjreflect_" << comp.class_name << "_fields[] = {\n";
    for (const auto& f : comp.fields) {
        out << "    {\"" << f.name << "\", " << f.fj_field_type << ", " << access << "offset_" << f.name
            << ", ";
        // An enum field's size is its label count, so the runtime can bound-check
        // without a separate count.
        if (f.enum_labels.empty()) out << access << "size_" << f.name;
        else out << f.enum_labels.size();
        out << ", ";
        if (f.enum_labels.empty()) {
            out << "nullptr";
        } else {
            out << label_symbol(f.name);
        }
        out << ", " << f.hint_min << "f, " << f.hint_max << "f, " << f.hint_speed << "f, ";
        if (f.ref_target.empty()) out << "nullptr";
        else out << "\"" << f.ref_target << "\"";
        out << ", ";
        if (f.tooltip.empty()) out << "nullptr";
        else out << "\"" << f.tooltip << "\"";
        out << ", ";
        if (f.category.empty()) out << "nullptr";
        else out << "\"" << f.category << "\"";
        out << ", " << (f.is_readonly ? 1 : 0);
        out << ", " << (f.is_replicated ? 1 : 0);
        out << ", ";
        if (f.shader_types.empty()) {
            out << "nullptr";
        } else {
            out << "\"";
            for (size_t t = 0; t < f.shader_types.size(); ++t) {
                if (t > 0) out << ",";
                out << f.shader_types[t];
            }
            out << "\"";
        }
        out << "},\n";
    }
    if (!comp.fields.empty()) out << "};\n\n";

    // Trace function
    if (kinds.any()) {
        out << "extern \"C\" void fjreflect_" << comp.class_name
            << "_trace(void* instance, void* visitor) {\n";
        out << "    char* base = static_cast<char*>(instance);\n";
        out << "    (void)visitor;\n";
        for (const auto& f : comp.fields) {
            const char* bridge = f.fj_field_type == "FJELL_FIELD_WEAK_REF"     ? "fjell_gc_trace_weak"
                                 : f.fj_field_type == "FJELL_FIELD_REF"        ? "fjell_gc_trace_ref"
                                 : f.fj_field_type == "FJELL_FIELD_SHARED_REF" ? "fjell_gc_trace_shared"
                                                                               : nullptr;
            if (bridge != nullptr) {
                out << "    " << bridge << "(visitor, base + " << access << "offset_" << f.name << ");\n";
            }
        }
        out << "}\n";
    } else {
        // No ref fields — emit a null-able trace (won't be registered)
        out << "// No reference fields — no trace function needed.\n";
    }

    // UI widget action trampolines (original format — void(void*) and void(void*,int))
    if (comp.is_ui_component && !comp.actions.empty()) {
        out << "\n// ── UI action trampolines ──\n\n";
        for (const auto& action : comp.actions) {
            if (action.has_int_param) {
                out << "static void fjreflect_" << comp.class_name << "_action_"
                    << action.method_name << "(void* self, int arg) {\n";
                out << "    static_cast<" << comp.qualified_name << "*>(self)->"
                    << action.method_name << "(arg);\n";
                out << "}\n\n";
            } else {
                out << "static void fjreflect_" << comp.class_name << "_action_"
                    << action.method_name << "(void* self) {\n";
                out << "    static_cast<" << comp.qualified_name << "*>(self)->"
                    << action.method_name << "();\n";
                out << "}\n\n";
            }
        }

        out << "extern \"C\" const uint32_t fjreflect_" << comp.class_name
            << "_action_count = " << comp.actions.size() << ";\n\n";

        // UI action descriptor table for runtime validation
        out << "extern \"C\" const FjellUiActionDesc fjreflect_" << comp.class_name
            << "_ui_actions[] = {\n";
        for (const auto& action : comp.actions) {
            out << "    {\"" << action.name << "\", ";
            switch (action.param) {
                case ActionParamType::none:        out << "FJELL_UI_ACTION_NONE"; break;
                case ActionParamType::int_param:   out << "FJELL_UI_ACTION_INT"; break;
                case ActionParamType::float_param: out << "FJELL_UI_ACTION_FLOAT"; break;
                case ActionParamType::vec2_param:  out << "FJELL_UI_ACTION_VEC2"; break;
            }
            out << "},\n";
        }
        out << "};\n";
        out << "extern \"C\" const uint32_t fjreflect_" << comp.class_name
            << "_ui_action_count = " << comp.actions.size() << ";\n\n";
    }

    // ── RPC invoke trampolines + descriptor table ──────────────────────
    if (!comp.rpcs.empty()) {
        out << "\n// ── RPC invoke trampolines ──\n\n";
        for (const auto& rpc : comp.rpcs) {
            // Deserialize POD params from `in` and call the handler via the shim.
            out << "static void fjreflect_" << comp.class_name << "_rpc_"
                << rpc.method_name << "_invoke(void* self, const uint8_t* in, uint32_t size) {\n";
            out << "    (void)in; (void)size;\n";
            out << "    uint32_t off = 0;\n";
            for (size_t p = 0; p < rpc.params.size(); ++p) {
                const auto& param = rpc.params[p];
                out << "    " << param.cpp_type << " a" << p << ";\n";
                out << "    std::memcpy(&a" << p << ", in + off, sizeof("
                    << param.cpp_type << ")); off += sizeof(" << param.cpp_type << ");\n";
            }
            out << "    " << access << rpc.method_name << "(static_cast<" << comp.qualified_name
                << "*>(self)";
            for (size_t p = 0; p < rpc.params.size(); ++p) {
                out << ", a" << p;
            }
            out << ");\n";
            out << "}\n\n";
        }

        out << "extern \"C\" const FjellRpcDesc fjreflect_" << comp.class_name
            << "_rpcs[] = {\n";
        for (const auto& rpc : comp.rpcs) {
            const char* tgt = rpc.target == RpcTarget::clients ? "FJELL_RPC_CLIENTS"
                            : rpc.target == RpcTarget::all     ? "FJELL_RPC_ALL"
                                                               : "FJELL_RPC_SERVER";
            // payload_size = sum of sizeof(param)
            out << "    {\"" << rpc.method_name << "\", " << tgt << ", ";
            if (rpc.params.empty()) {
                out << "0";
            } else {
                for (size_t p = 0; p < rpc.params.size(); ++p) {
                    out << "sizeof(" << rpc.params[p].cpp_type << ")";
                    if (p + 1 < rpc.params.size()) out << " + ";
                }
            }
            out << ", fjreflect_" << comp.class_name << "_rpc_" << rpc.method_name
                << "_invoke},\n";
        }
        out << "};\n";
        out << "extern \"C\" const uint32_t fjreflect_" << comp.class_name
            << "_rpc_count = " << comp.rpcs.size() << ";\n\n";

        // ── Send-wrapper member definitions ────────────────────────────
        // The user declares `void method(args);` ([[fj::rpc]]) and writes the
        // handler as `method_impl(args)`. We define `method(args)` here: it
        // serializes the POD args and hands them to NetAPI by (node id, rpc
        // index). On the target peer the invoke trampoline calls method_impl.
        out << "// ── RPC send wrappers ──\n\n";
        for (size_t r = 0; r < comp.rpcs.size(); ++r) {
            const auto& rpc = comp.rpcs[r];
            out << "void " << comp.qualified_name << "::" << rpc.method_name << "(";
            for (size_t p = 0; p < rpc.params.size(); ++p) {
                out << rpc.params[p].cpp_type << " " << rpc.params[p].name;
                if (p + 1 < rpc.params.size()) out << ", ";
            }
            out << ") {\n";
            // pack args into a stack buffer
            if (rpc.params.empty()) {
                out << "    fjell::NetAPI::send_rpc(id(), " << r << ", nullptr, 0);\n";
            } else {
                out << "    uint8_t buf[";
                for (size_t p = 0; p < rpc.params.size(); ++p) {
                    out << "sizeof(" << rpc.params[p].cpp_type << ")";
                    if (p + 1 < rpc.params.size()) out << " + ";
                }
                out << "];\n";
                out << "    uint32_t off = 0;\n";
                for (size_t p = 0; p < rpc.params.size(); ++p) {
                    out << "    std::memcpy(buf + off, &" << rpc.params[p].name
                        << ", sizeof(" << rpc.params[p].cpp_type << ")); off += sizeof("
                        << rpc.params[p].cpp_type << ");\n";
                }
                out << "    fjell::NetAPI::send_rpc(id(), " << r << ", buf, off);\n";
            }
            out << "}\n\n";
        }
    }
}

/// `<stem>.generated.cpp`: every component its header declares.
std::string generated_file(const std::string& stem, const std::vector<const ComponentInfo*>& comps) {
    std::ostringstream out;
    out << "// " << stem << ".generated.cpp — Generated by fjreflect. Do not edit.\n";
    out << "#include \"fjell/api.h\"\n";
    auto source_filename = std::filesystem::path(comps.front()->source_file).filename().string();
    out << "#include \"" << source_filename << "\"\n";
    const bool has_rpcs = std::any_of(comps.begin(), comps.end(), [](const ComponentInfo* c) {
        return !c->rpcs.empty();
    });
    if (has_rpcs) {
        out << "#include \"fjell/net_api.hpp\"\n";
    }
    out << "#include <cstddef>\n";
    out << "#include <cstdint>\n";
    out << "#include <cstring>\n\n";
    // Components are not standard-layout (they have a base and virtuals), for
    // which offsetof is conditionally supported: GCC, Clang and MSVC all
    // support it for single, non-virtual inheritance, and GCC warns.
    out << "#if defined(__GNUC__)\n#pragma GCC diagnostic ignored \"-Winvalid-offsetof\"\n#endif\n\n";

    // Declare trace bridge functions if needed
    RefKinds kinds;
    for (const auto* comp : comps) {
        const auto k = ref_kinds(*comp);
        kinds.weak = kinds.weak || k.weak;
        kinds.ref = kinds.ref || k.ref;
        kinds.shared = kinds.shared || k.shared;
    }
    if (kinds.weak) out << "extern \"C\" void fjell_gc_trace_weak(void*, void*);\n";
    if (kinds.ref) out << "extern \"C\" void fjell_gc_trace_ref(void*, void*);\n";
    if (kinds.shared) out << "extern \"C\" void fjell_gc_trace_shared(void*, void*);\n";
    if (kinds.any()) out << "\n";

    for (std::size_t i = 0; i < comps.size(); ++i) {
        if (i > 0) out << "\n";
        emit_component(out, *comps[i]);
    }
    return out.str();
}

/// Output files fjreflect owns in its directory, including the `.verify.cpp`
/// files it once wrote; anything else there is left alone.
bool is_generated_name(const std::string& name) {
    return name == "manifest.json" || name.ends_with(".generated.cpp") ||
           name.ends_with(".verify.cpp");
}

const char* field_kind(const FieldInfo& f) {
    static const std::pair<const char*, const char*> kinds[] = {
        {"FJELL_FIELD_FLOAT", "float"},       {"FJELL_FIELD_INT", "int"},
        {"FJELL_FIELD_BOOL", "bool"},         {"FJELL_FIELD_VEC2", "vec2"},
        {"FJELL_FIELD_VEC3", "vec3"},         {"FJELL_FIELD_VEC4", "vec4"},
        {"FJELL_FIELD_COLOR3", "color3"},     {"FJELL_FIELD_COLOR4", "color4"},
        {"FJELL_FIELD_ENUM", "enum"},         {"FJELL_FIELD_WEAK_REF", "weak_ref"},
        {"FJELL_FIELD_REF", "ref"},           {"FJELL_FIELD_SHARED_REF", "shared_ref"},
        {"FJELL_FIELD_WIDGET_PTR", "widget_ptr"}, {"FJELL_FIELD_STD_STRING", "string"},
    };
    for (const auto& [name, kind] : kinds) {
        if (f.fj_field_type == name) return kind;
    }
    return "unknown";
}

const char* param_name(ActionParamType param) {
    switch (param) {
        case ActionParamType::none:        return "none";
        case ActionParamType::int_param:   return "int";
        case ActionParamType::float_param: return "float";
        case ActionParamType::vec2_param:  return "vec2";
    }
    return "none";
}

const char* trigger_name(ActionTriggerMode trigger) {
    switch (trigger) {
        case ActionTriggerMode::triggered: return "triggered";
        case ActionTriggerMode::pressed:   return "pressed";
        case ActionTriggerMode::released:  return "released";
        case ActionTriggerMode::completed: return "completed";
    }
    return "triggered";
}

const char* target_name(RpcTarget target) {
    switch (target) {
        case RpcTarget::server:  return "server";
        case RpcTarget::clients: return "clients";
        case RpcTarget::all:     return "all";
    }
    return "server";
}

/// The value of a C++ string literal's body: fields keep tooltips, categories
/// and labels as written, because the generated code writes them back into
/// literals, but a tool reading the manifest wants the text itself.
std::string unescape(const std::string& body) {
    std::string out;
    for (std::size_t i = 0; i < body.size(); ++i) {
        if (body[i] != '\\' || i + 1 == body.size()) {
            out += body[i];
            continue;
        }
        switch (const char c = body[++i]) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            default: out += c; break; // \\ \" \' and anything rarer, as itself
        }
    }
    return out;
}

/// Everything fjreflect read, for tools that check a project against its
/// scripts (the lint, the editor). Nothing in the build reads it.
std::string manifest(const std::vector<ComponentInfo>& components) {
    using Json = nlohmann::ordered_json;
    Json types = Json::array();
    for (const auto& comp : components) {
        Json type;
        type["name"] = comp.class_name;
        type["qualified_name"] = comp.qualified_name;
        type["kind"] = comp.is_ui_component ? "ui_widget" : comp.is_notify ? "notify" : "component";
        type["header"] = std::filesystem::path(comp.source_file).filename().string();
        Json fields = Json::array();
        for (const auto& f : comp.fields) {
            Json field;
            field["name"] = f.name;
            field["type"] = f.cpp_type;
            field["kind"] = field_kind(f);
            field["line"] = f.line;
            if (f.is_bind) field["bind"] = true;
            if (f.is_readonly) field["readonly"] = true;
            if (f.is_replicated) field["replicated"] = true;
            if (f.has_range) field["range"] = {f.hint_min, f.hint_max};
            if (f.hint_speed != 0.0f) field["step"] = f.hint_speed;
            if (!f.tooltip.empty()) field["tooltip"] = unescape(f.tooltip);
            if (!f.category.empty()) field["category"] = unescape(f.category);
            if (!f.enum_labels.empty()) {
                Json labels = Json::array();
                for (const auto& label : f.enum_labels) labels.push_back(unescape(label));
                field["enum"] = std::move(labels);
            }
            if (!f.ref_target.empty()) field["ref"] = f.ref_target;
            if (!f.shader_types.empty()) field["shader_types"] = f.shader_types;
            fields.push_back(std::move(field));
        }
        type["fields"] = std::move(fields);
        Json actions = Json::array();
        for (const auto& a : comp.actions) {
            actions.push_back({{"name", a.name}, {"param", param_name(a.param)}, {"trigger", trigger_name(a.trigger)}});
        }
        type["actions"] = std::move(actions);
        Json rpcs = Json::array();
        for (const auto& rpc : comp.rpcs) {
            Json params = Json::array();
            for (const auto& p : rpc.params) params.push_back({{"type", p.cpp_type}, {"name", p.name}});
            rpcs.push_back({{"name", rpc.method_name}, {"target", target_name(rpc.target)}, {"params", std::move(params)}});
        }
        type["rpcs"] = std::move(rpcs);
        types.push_back(std::move(type));
    }
    Json root;
    root["version"] = 1;
    root["types"] = std::move(types);
    return root.dump(2) + "\n";
}

} // namespace

std::string register_file(const std::vector<ComponentInfo>& components) {
    std::ostringstream out;
    out << "// register_all.cpp — Generated by fjreflect. Do not edit.\n";
    out << "#include \"fjell/registry.hpp\"\n";

    std::vector<const ComponentInfo*> scripts, widgets;
    for (const auto& comp : components) (comp.is_ui_component ? widgets : scripts).push_back(&comp);
    if (!widgets.empty()) out << "#include \"fjell/ui_registry.hpp\"\n";
    out << "\n";

    for (const auto& comp : components) {
        const auto& n = comp.class_name;
        if (!comp.fields.empty()) out << "extern \"C\" const FjellFieldDesc fjreflect_" << n << "_fields[];\n";
        if (ref_kinds(comp).any()) out << "extern \"C\" void fjreflect_" << n << "_trace(void*, void*);\n";
        if (!comp.rpcs.empty()) out << "extern \"C\" const FjellRpcDesc fjreflect_" << n << "_rpcs[];\n";
        if (comp.is_ui_component && !comp.actions.empty()) {
            out << "extern \"C\" const FjellUiActionDesc fjreflect_" << n << "_ui_actions[];\n";
        }
    }

    auto fields = [](const ComponentInfo& comp) {
        return comp.fields.empty() ? std::string("nullptr") : "fjreflect_" + comp.class_name + "_fields";
    };
    auto trace = [](const ComponentInfo& comp) {
        return ref_kinds(comp).any() ? "fjreflect_" + comp.class_name + "_trace" : std::string("nullptr");
    };
    if (!scripts.empty()) {
        out << "\nstatic const FjellGeneratedInfo fjreflect_generated[] = {\n";
        for (const auto* comp : scripts) {
            const auto& n = comp->class_name;
            out << "    {\"" << n << "\", " << fields(*comp) << ", " << comp->fields.size() << ", "
                << trace(*comp) << ", ";
            if (comp->rpcs.empty()) out << "nullptr, 0";
            else out << "fjreflect_" << n << "_rpcs, " << comp->rpcs.size();
            out << "},\n";
        }
        out << "};\n";
    }
    if (!widgets.empty()) {
        out << "\nstatic const FjellUiGeneratedInfo fjreflect_ui_generated[] = {\n";
        for (const auto* comp : widgets) {
            const auto& n = comp->class_name;
            out << "    {\"" << n << "\", " << fields(*comp) << ", " << comp->fields.size() << ", "
                << trace(*comp) << ", ";
            if (comp->actions.empty()) out << "nullptr, 0";
            else out << "fjreflect_" << n << "_ui_actions, " << comp->actions.size();
            out << "},\n";
        }
        out << "};\n";
    }

    // FJELL_REGISTER()'s entry points, with the generated tables handed over
    // before the types register.
    out << "\nextern \"C\" FJELL_GAME_EXPORT uint32_t fjell_api_version() { return FJELL_API_VERSION; }\n";
    out << "FJELL_DEFINE_ENGINE_COMPONENT_LAYOUTS()\n";
    out << "extern \"C\" FJELL_GAME_EXPORT void fjell_register(FjellTypeRegistry* registry) {\n";
    if (!scripts.empty()) {
        out << "    fjell_set_generated_table(fjreflect_generated, " << scripts.size() << ");\n";
    }
    if (!widgets.empty()) {
        out << "    fjell_set_ui_generated_table(fjreflect_ui_generated, " << widgets.size() << ");\n";
    }
    out << "    for (auto* r = fjell_auto_reg_head(); r; r = r->next) r->fn(registry);\n";
    out << "    for (auto* r = fjell_ui_auto_reg_head(); r; r = r->next) r->fn(registry);\n";
    out << "    for (auto* r = fjell_notify_auto_reg_head(); r; r = r->next) r->fn(registry);\n";
    out << "}\n";
    return out.str();
}

GeneratedFiles generate(const std::vector<ComponentInfo>& components) {
    // Components grouped by the header that declares them, in the order the
    // headers were given.
    std::vector<std::pair<std::string, std::vector<const ComponentInfo*>>> by_header;
    for (const auto& comp : components) {
        const auto stem = stem_of(comp);
        auto it = std::find_if(by_header.begin(), by_header.end(),
                               [&](const auto& entry) { return entry.first == stem; });
        if (it == by_header.end()) {
            by_header.emplace_back(stem, std::vector<const ComponentInfo*>{});
            it = std::prev(by_header.end());
        }
        it->second.push_back(&comp);
    }

    GeneratedFiles files;
    for (const auto& [stem, comps] : by_header) {
        files[stem + ".generated.cpp"] = generated_file(stem, comps);
    }
    files["manifest.json"] = manifest(components);
    return files;
}

WriteResult write_if_changed(const std::string& path, const std::string& content) {
    std::ifstream in(path, std::ios::binary);
    if (in) {
        const std::string existing((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (existing == content) return WriteResult::unchanged;
    }
    in.close();
    std::ofstream out(path, std::ios::binary);
    out << content;
    if (!out) {
        std::cerr << "fjreflect: cannot write " << path << "\n";
        return WriteResult::failed;
    }
    return WriteResult::written;
}

bool write_outputs(const std::string& output_dir, const GeneratedFiles& files, WriteReport& report) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(output_dir, ec);

    bool ok = true;
    for (const auto& [name, content] : files) {
        switch (write_if_changed((fs::path(output_dir) / name).string(), content)) {
            case WriteResult::unchanged: break;
            case WriteResult::written: report.written.push_back(name); break;
            case WriteResult::failed: ok = false; break;
        }
    }

    // A header that was deleted or renamed, or that no longer declares a
    // component, leaves outputs behind that the game's build would still
    // compile.
    std::vector<fs::path> stale;
    for (const auto& entry : fs::directory_iterator(output_dir, ec)) {
        const auto name = entry.path().filename().string();
        if (entry.is_regular_file() && is_generated_name(name) && !files.contains(name)) {
            stale.push_back(entry.path());
        }
    }
    for (const auto& path : stale) {
        if (!fs::remove(path, ec) || ec) {
            std::cerr << "fjreflect: cannot remove " << path.string() << "\n";
            ok = false;
            continue;
        }
        report.removed.push_back(path.filename().string());
    }
    return ok;
}

} // namespace fjreflect
