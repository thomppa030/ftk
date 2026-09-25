#include "generator.hpp"

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

/// One component's field table, trace function, action trampolines and RPC
/// code, in that order.
void emit_component(std::ostream& out, const ComponentInfo& comp) {
    const auto kinds = ref_kinds(comp);
    const bool has_weak = kinds.weak, has_ref = kinds.ref, has_shared = kinds.shared;

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

    // Field table
    out << "extern \"C\" const FjellFieldDesc fjreflect_" << comp.class_name << "_fields[] = {\n";
    for (const auto& f : comp.fields) {
        // For enum fields, override size with label count so the runtime can
        // bound-check without a separate count field.
        uint32_t size = f.enum_labels.empty() ? f.size
                                              : static_cast<uint32_t>(f.enum_labels.size());

        out << "    {\"" << f.name << "\", " << f.fj_field_type
            << ", " << f.offset << ", " << size << ", ";
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
    out << "};\n\n";

    // Field count
    out << "extern \"C\" const uint32_t fjreflect_" << comp.class_name
        << "_field_count = " << comp.fields.size() << ";\n\n";

    // Trace function
    bool has_refs = has_weak || has_ref || has_shared;
    if (has_refs) {
        out << "extern \"C\" void fjreflect_" << comp.class_name
            << "_trace(void* instance, void* visitor) {\n";
        out << "    char* base = static_cast<char*>(instance);\n";
        out << "    (void)visitor;\n";
        for (const auto& f : comp.fields) {
            if (f.fj_field_type == "FJELL_FIELD_WEAK_REF") {
                out << "    fjell_gc_trace_weak(visitor, base + " << f.offset << ");\n";
            } else if (f.fj_field_type == "FJELL_FIELD_REF") {
                out << "    fjell_gc_trace_ref(visitor, base + " << f.offset << ");\n";
            } else if (f.fj_field_type == "FJELL_FIELD_SHARED_REF") {
                out << "    fjell_gc_trace_shared(visitor, base + " << f.offset << ");\n";
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

    // (ScriptComponent action trampolines removed — scripts now use InputAPI::bind() in begin_play)

    // ── RPC invoke trampolines + descriptor table ──────────────────────
    if (!comp.rpcs.empty()) {
        // Access shim: a friend of the component (via FJELL_COMPONENT), so the
        // generated trampolines can call private _impl handlers.
        out << "\n// ── RPC access shim (friend of " << comp.class_name << ") ──\n\n";
        out << "namespace fjell { template <> struct FjellRpcAccess<"
            << comp.qualified_name << "> {\n";
        for (const auto& rpc : comp.rpcs) {
            out << "    static void " << rpc.method_name << "(" << comp.qualified_name << "* self";
            for (size_t p = 0; p < rpc.params.size(); ++p)
                out << ", " << rpc.params[p].cpp_type << " a" << p;
            out << ") { self->" << rpc.method_name << "_impl(";
            for (size_t p = 0; p < rpc.params.size(); ++p) {
                out << "a" << p;
                if (p + 1 < rpc.params.size()) out << ", ";
            }
            out << "); }\n";
        }
        out << "}; }\n\n";

        out << "// ── RPC invoke trampolines ──\n\n";
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
            out << "    fjell::FjellRpcAccess<" << comp.qualified_name << ">::"
                << rpc.method_name << "(static_cast<" << comp.qualified_name << "*>(self)";
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
    const bool calls_members = std::any_of(comps.begin(), comps.end(), [](const ComponentInfo* c) {
        return !c->actions.empty() || !c->rpcs.empty();
    });
    const bool has_rpcs = std::any_of(comps.begin(), comps.end(), [](const ComponentInfo* c) {
        return !c->rpcs.empty();
    });
    if (calls_members) {
        // Include source header for action/RPC trampoline static_casts and the
        // generated RPC send-wrapper member definitions.
        auto source_filename = std::filesystem::path(comps.front()->source_file).filename().string();
        out << "#include \"" << source_filename << "\"\n";
    }
    if (has_rpcs) {
        out << "#include \"fjell/net_api.hpp\"\n";
    }
    out << "#include <cstdint>\n";
    out << "#include <cstring>\n\n";

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

/// `<stem>.verify.cpp`: static assertions that the offsets still hold.
std::string verify_file(const std::string& stem, const std::vector<const ComponentInfo*>& comps) {
    std::ostringstream out;
    out << "// " << stem << ".verify.cpp — Layout verification. Generated by fjreflect.\n";
    auto source_filename = std::filesystem::path(comps.front()->source_file).filename().string();
    out << "#include \"" << source_filename << "\"\n";
    out << "#include <cstddef>\n\n";

    for (const auto* comp : comps) {
        for (const auto& f : comp->fields) {
            out << "static_assert(offsetof(" << comp->qualified_name << ", " << f.name
                << ") == " << f.offset << ", \"fjreflect layout mismatch: "
                << comp->qualified_name << "::" << f.name << "\");\n";
        }
    }
    return out.str();
}

/// Output files fjreflect owns in its directory; anything else there is left alone.
bool is_generated_name(const std::string& name) {
    return name == "manifest.json" || name.ends_with(".generated.cpp") ||
           name.ends_with(".verify.cpp");
}

std::string manifest(const std::vector<ComponentInfo>& components) {
    std::ostringstream out;
    out << "{\n  \"types\": [\n";
    for (size_t i = 0; i < components.size(); ++i) {
        const auto& comp = components[i];
        bool has_refs = false;
        for (const auto& f : comp.fields) {
            if (f.fj_field_type.find("REF") != std::string::npos) {
                has_refs = true;
                break;
            }
        }

        out << "    {\n";
        out << "      \"name\": \"" << comp.class_name << "\",\n";
        if (comp.is_ui_component) {
            out << "      \"is_ui_component\": true,\n";
        }
        if (comp.is_notify) {
            out << "      \"is_notify\": true,\n";
        }
        out << "      \"fields_symbol\": \"fjreflect_" << comp.class_name << "_fields\",\n";
        out << "      \"field_count\": " << comp.fields.size() << ",\n";
        if (has_refs) {
            out << "      \"trace_symbol\": \"fjreflect_" << comp.class_name << "_trace\",\n";
        } else {
            out << "      \"trace_symbol\": null,\n";
        }
        out << "      \"action_count\": " << comp.actions.size() << ",\n";
        // Action details for runtime table generation
        out << "      \"actions\": [";
        for (size_t a = 0; a < comp.actions.size(); ++a) {
            const auto& action = comp.actions[a];
            int param_type = 0;
            switch (action.param) {
                case ActionParamType::none:        param_type = 0; break;
                case ActionParamType::int_param:   param_type = 1; break;
                case ActionParamType::float_param: param_type = 2; break;
                case ActionParamType::vec2_param:  param_type = 3; break;
            }
            out << "{\"name\":\"" << action.name << "\",\"param\":" << param_type << "}";
            if (a + 1 < comp.actions.size()) out << ",";
        }
        out << "],\n";
        out << "      \"rpc_count\": " << comp.rpcs.size() << "\n";
        out << "    }";
        if (i + 1 < components.size()) out << ",";
        out << "\n";
    }
    out << "  ]\n}\n";
    return out.str();
}


} // namespace

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
        files[stem + ".verify.cpp"] = verify_file(stem, comps);
    }
    files["manifest.json"] = manifest(components);
    return files;
}

bool write_outputs(const std::string& output_dir, const GeneratedFiles& files, WriteReport& report) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(output_dir, ec);

    bool ok = true;
    for (const auto& [name, content] : files) {
        const auto path = fs::path(output_dir) / name;
        std::ifstream in(path, std::ios::binary);
        if (in) {
            const std::string existing((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (existing == content) continue;
        }
        in.close();
        std::ofstream out(path, std::ios::binary);
        out << content;
        if (!out) {
            std::cerr << "fjreflect: cannot write " << path.string() << "\n";
            ok = false;
            continue;
        }
        report.written.push_back(name);
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
