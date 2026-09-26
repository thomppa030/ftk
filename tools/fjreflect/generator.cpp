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

/// A qualified name as part of a C identifier: `game::Slot` → `game__Slot`.
std::string mangle(const std::string& qualified) {
    std::string out;
    for (std::size_t i = 0; i < qualified.size(); ++i) {
        if (qualified.compare(i, 2, "::") == 0) {
            out += "__";
            ++i;
        } else {
            out += qualified[i];
        }
    }
    return out;
}

std::string type_symbol(const std::string& qualified) { return "fjreflect_type_" + mangle(qualified); }

std::string stem_of(const std::string& source_file) {
    return std::filesystem::path(source_file).stem().string();
}

std::string flags_of(const FieldInfo& f) {
    std::string out;
    auto add = [&](const char* flag) { out += (out.empty() ? "" : " | ") + std::string(flag); };
    if (f.is_state) add("FJELL_FIELD_FLAG_STATE");
    if (f.is_getter) add("FJELL_FIELD_FLAG_GETTER");
    if (f.is_model) add("FJELL_FIELD_FLAG_MODEL");
    return out.empty() ? "0" : out;
}

/// The friend every component macro declares, specialised for a reflected
/// class: the offset and size of each field, measured by the compiler that
/// builds the game with the game's own flags; trampolines for getters and for
/// reading and editing vectors; and the shims through which RPCs and UI
/// actions reach private handlers.
void emit_access(std::ostream& out, const std::string& q, const std::vector<FieldInfo>& fields,
                 const std::vector<RpcInfo>& rpcs, const std::vector<ActionInfo>& actions) {
    if (fields.empty() && rpcs.empty() && actions.empty()) return;
    out << "namespace fjell { template <> struct FjellAccess<" << q << "> {\n";
    for (const auto& f : fields) {
        if (f.is_getter) {
            out << "    static constexpr uint32_t size_" << f.name << " = static_cast<uint32_t>(sizeof("
                << f.cpp_type << "));\n";
            out << "    static void get_" << f.name << "(const void* self, void* out) { *static_cast<"
                << f.cpp_type << "*>(out) = static_cast<const " << q << "*>(self)->" << f.name << "(); }\n";
            continue;
        }
        out << "    static constexpr uint32_t offset_" << f.name << " = static_cast<uint32_t>(offsetof("
            << q << ", " << f.name << "));\n";
        out << "    static constexpr uint32_t size_" << f.name << " = static_cast<uint32_t>(sizeof("
            << q << "::" << f.name << "));\n";
        if (f.fj_field_type == "FJELL_FIELD_VECTOR") {
            const auto list = "decltype(" + q + "::" + f.name + ")";
            out << "    static constexpr uint32_t element_size_" << f.name << " = static_cast<uint32_t>(sizeof("
                << list << "::value_type));\n";
            out << "    static uint32_t length_" << f.name << "(const void* v) { return static_cast<uint32_t>("
                << "static_cast<const " << list << "*>(v)->size()); }\n";
            out << "    static const void* item_" << f.name << "(const void* v, uint32_t i) { return &(*static_cast<const "
                << list << "*>(v))[i]; }\n";
            auto at = [](const char* index) {
                return "l.begin() + static_cast<std::ptrdiff_t>(" + std::string(index) + ")";
            };
            out << "    static void insert_" << f.name << "(void* v, uint32_t i) { auto& l = *static_cast<" << list
                << "*>(v); l.insert(" << at("i") << ", " << list << "::value_type{}); }\n";
            out << "    static void erase_" << f.name << "(void* v, uint32_t i) { auto& l = *static_cast<" << list
                << "*>(v); l.erase(" << at("i") << "); }\n";
            out << "    static void move_" << f.name << "(void* v, uint32_t from, uint32_t to) { auto& l = *static_cast<"
                << list << "*>(v); if (from < to) std::rotate(" << at("from") << ", " << at("from") << " + 1, "
                << at("to") << " + 1); else if (to < from) std::rotate(" << at("to") << ", " << at("from") << ", "
                << at("from") << " + 1); }\n";
        }
    }
    // A UI action is called with its argument behind a pointer, the one
    // signature the engine calls every action through.
    for (const auto& action : actions) {
        const bool takes_arg = action.param != ActionParamType::none;
        out << "    static void action_" << action.method_name << "(void* self, const void*"
            << (takes_arg ? " arg" : "") << ") { ";
        out << "static_cast<" << q << "*>(self)->" << action.method_name << "(";
        switch (action.param) {
            case ActionParamType::none: break;
            case ActionParamType::int_param: out << "*static_cast<const int*>(arg)"; break;
            case ActionParamType::float_param: out << "*static_cast<const float*>(arg)"; break;
            case ActionParamType::vec2_param: out << "*static_cast<const glm::vec2*>(arg)"; break;
        }
        out << "); }\n";
    }
    // Handlers are private `_impl` methods more often than not.
    for (const auto& rpc : rpcs) {
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

/// Symbols a class's tables are written under: the component's own names,
/// which register_all.cpp and the engine know, or a struct's mangled ones.
struct TableNames {
    std::string access;        // "fjell::FjellAccess<Q>::"
    std::string labels_prefix; // + field name + "_labels"
    std::string vector_prefix; // + field name
};

/// A vector field's element description, read and edited through its
/// trampolines.
void emit_vector_descs(std::ostream& out, const std::vector<FieldInfo>& fields, const TableNames& names) {
    bool any = false;
    for (const auto& f : fields) {
        if (f.fj_field_type != "FJELL_FIELD_VECTOR") continue;
        any = true;
        out << "static const FjellVectorDesc " << names.vector_prefix << f.name << " = {" << f.element_fj_type
            << ", ";
        if (f.element_fj_type == "FJELL_FIELD_STRUCT") out << "&" << type_symbol(f.element_struct);
        else out << "nullptr";
        out << ", " << names.access << "element_size_" << f.name << ", " << names.access << "length_" << f.name
            << ", " << names.access << "item_" << f.name << ", " << names.access << "insert_" << f.name << ", "
            << names.access << "erase_" << f.name << ", " << names.access << "move_" << f.name << "};\n";
    }
    if (any) out << "\n";
}

void emit_labels(std::ostream& out, const std::vector<FieldInfo>& fields, const TableNames& names) {
    bool any = false;
    for (const auto& f : fields) {
        if (f.enum_labels.empty()) continue;
        any = true;
        out << "static const char* const " << names.labels_prefix << f.name << "_labels[] = {";
        for (std::size_t e = 0; e < f.enum_labels.size(); ++e) {
            if (e) out << ", ";
            out << "\"" << f.enum_labels[e] << "\"";
        }
        out << "};\n";
    }
    if (any) out << "\n";
}

/// One `FjellFieldDesc` initializer per field.
void emit_field_entries(std::ostream& out, const std::vector<FieldInfo>& fields, const TableNames& names) {
    out << std::fixed;
    for (const auto& f : fields) {
        out << "    {\"" << f.name << "\", " << f.fj_field_type << ", ";
        if (f.is_getter) out << "0"; // nothing is stored: the value comes from `get`
        else out << names.access << "offset_" << f.name;
        out << ", ";
        // An enum field's size is its label count, so the runtime can bound-check
        // without a separate count.
        if (f.enum_labels.empty()) out << names.access << "size_" << f.name;
        else out << f.enum_labels.size();
        out << ", ";
        if (f.enum_labels.empty()) out << "nullptr";
        else out << names.labels_prefix << f.name << "_labels";
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
        out << ", " << flags_of(f) << ", ";
        if (f.fj_field_type == "FJELL_FIELD_STRUCT") out << "&" << type_symbol(f.struct_type);
        else out << "nullptr";
        out << ", ";
        if (f.fj_field_type == "FJELL_FIELD_VECTOR") out << "&" << names.vector_prefix << f.name;
        else out << "nullptr";
        out << ", ";
        if (f.is_getter) out << names.access << "get_" << f.name;
        else out << "nullptr";
        out << "},\n";
    }
}

/// One component's field table, trace function, action trampolines and RPC
/// code, in that order.
void emit_component(std::ostream& out, const ComponentInfo& comp) {
    const auto kinds = ref_kinds(comp);
    const TableNames names{"fjell::FjellAccess<" + comp.qualified_name + ">::", "fjreflect_" + comp.class_name + "_",
                           "fjreflect_vector_" + mangle(comp.qualified_name) + "_"};
    const auto& access = names.access;

    emit_access(out, comp.qualified_name, comp.fields, comp.rpcs, comp.actions);
    emit_labels(out, comp.fields, names);
    emit_vector_descs(out, comp.fields, names);

    // Field table. A class without fields has none: a zero-length array is not
    // C++, and register_all.cpp passes nullptr for it.
    if (!comp.fields.empty()) {
        out << "extern \"C\" const FjellFieldDesc fjreflect_" << comp.class_name << "_fields[] = {\n";
        emit_field_entries(out, comp.fields, names);
        out << "};\n\n";
    }

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
    // UI widget actions: what markup may call, and how.
    if (comp.is_ui_component && !comp.actions.empty()) {
        out << "\n";
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
            out << ", &fjell::FjellAccess<" << comp.qualified_name << ">::action_" << action.method_name << "},\n";
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

/// A reflected struct's field table and type description, which the engine
/// registers before the components that hold it.
void emit_struct(std::ostream& out, const StructInfo& info) {
    const auto& q = info.qualified_name;
    const auto symbol = mangle(q);
    const TableNames names{"fjell::FjellAccess<" + q + ">::", "fjreflect_" + symbol + "_",
                           "fjreflect_vector_" + symbol + "_"};

    emit_access(out, q, info.fields, {}, {});
    emit_vector_descs(out, info.fields, names);
    if (!info.fields.empty()) {
        out << "static const FjellFieldDesc fjreflect_fields_" << symbol << "[] = {\n";
        emit_field_entries(out, info.fields, names);
        out << "};\n\n";
    }
    out << "static_assert(std::is_default_constructible_v<" << q << ">, \"fjreflect: " << info.name
        << " is a Model's struct, which the editor makes with its default constructor\");\n";
    out << "extern \"C\" const FjellTypeDesc " << type_symbol(q) << " = {\"" << info.name << "\", \"" << q
        << "\", sizeof(" << q << "), alignof(" << q << "), ";
    if (info.fields.empty()) out << "nullptr, 0, ";
    else out << "fjreflect_fields_" << symbol << ", " << info.fields.size() << ", ";
    out << "[](void* at) { ::new (at) " << q << "(); }, [](void* at) { std::destroy_at(static_cast<" << q
        << "*>(at)); }};\n\n";
}

/// A pipe's trampoline: the engine hands it the value and the literal
/// arguments by address and gets the text back in a std::string.
void emit_pipe(std::ostream& out, const PipeInfo& pipe) {
    out << "extern \"C\" void fjreflect_pipe_" << pipe.name
        << "(const void* input, const void* const* args, void* out) {\n";
    out << "    (void)args;\n";
    out << "    *static_cast<std::string*>(out) = " << pipe.qualified_name << "(*static_cast<const "
        << pipe.input_cpp_type << "*>(input)";
    for (std::size_t i = 0; i < pipe.arg_cpp_types.size(); ++i) {
        out << ", *static_cast<const " << pipe.arg_cpp_types[i] << "*>(args[" << i << "])";
    }
    out << ");\n}\n\n";
}

/// Everything one header contributes.
struct HeaderOutput {
    std::string source_file;
    std::vector<const ComponentInfo*> components;
    std::vector<const StructInfo*> structs;
    std::vector<const PipeInfo*> pipes;
};

/// `<stem>.generated.cpp`: every component, reflected struct and pipe its
/// header declares.
std::string generated_file(const std::string& stem, const HeaderOutput& header) {
    std::ostringstream out;
    out << "// " << stem << ".generated.cpp — Generated by fjreflect. Do not edit.\n";
    out << "#include \"fjell/api.h\"\n";
    out << "#include \"" << std::filesystem::path(header.source_file).filename().string() << "\"\n";
    const bool has_rpcs = std::any_of(header.components.begin(), header.components.end(),
                                      [](const ComponentInfo* c) { return !c->rpcs.empty(); });
    if (has_rpcs) out << "#include \"fjell/net_api.hpp\"\n";
    // A list is reordered with std::rotate.
    auto has_list = [](const std::vector<FieldInfo>& fields) {
        return std::any_of(fields.begin(), fields.end(),
                           [](const FieldInfo& f) { return f.fj_field_type == "FJELL_FIELD_VECTOR"; });
    };
    const bool has_lists =
        std::any_of(header.components.begin(), header.components.end(),
                    [&](const ComponentInfo* c) { return has_list(c->fields); }) ||
        std::any_of(header.structs.begin(), header.structs.end(), [&](const StructInfo* s) { return has_list(s->fields); });
    if (has_lists) out << "#include <algorithm>\n";
    out << "#include <cstddef>\n";
    out << "#include <cstdint>\n";
    out << "#include <cstring>\n";
    if (!header.structs.empty()) out << "#include <memory>\n#include <new>\n#include <type_traits>\n";
    if (!header.pipes.empty()) out << "#include <string>\n";
    out << "\n";
    // Components are not standard-layout (they have a base and virtuals), for
    // which offsetof is conditionally supported: GCC, Clang and MSVC all
    // support it for single, non-virtual inheritance, and GCC warns.
    out << "#if defined(__GNUC__)\n#pragma GCC diagnostic ignored \"-Winvalid-offsetof\"\n#endif\n\n";
    // A header of plain Model structs includes nothing from the SDK, and the
    // tables are specialisations of this.
    if (!header.structs.empty()) out << "namespace fjell { template <class T> struct FjellAccess; }\n\n";

    // Declare trace bridge functions if needed
    RefKinds kinds;
    for (const auto* comp : header.components) {
        const auto k = ref_kinds(*comp);
        kinds.weak = kinds.weak || k.weak;
        kinds.ref = kinds.ref || k.ref;
        kinds.shared = kinds.shared || k.shared;
    }
    if (kinds.weak) out << "extern \"C\" void fjell_gc_trace_weak(void*, void*);\n";
    if (kinds.ref) out << "extern \"C\" void fjell_gc_trace_ref(void*, void*);\n";
    if (kinds.shared) out << "extern \"C\" void fjell_gc_trace_shared(void*, void*);\n";
    if (kinds.any()) out << "\n";

    // Every struct description this file names, wherever it is defined: a
    // table may name one defined further down, or in another header's file.
    std::vector<std::string> named;
    auto name_struct = [&](const std::string& q) {
        if (!q.empty() && std::find(named.begin(), named.end(), q) == named.end()) named.push_back(q);
    };
    auto name_fields = [&](const std::vector<FieldInfo>& fields) {
        for (const auto& f : fields) {
            if (f.fj_field_type == "FJELL_FIELD_STRUCT") name_struct(f.struct_type);
            if (f.element_fj_type == "FJELL_FIELD_STRUCT") name_struct(f.element_struct);
        }
    };
    for (const auto* comp : header.components) name_fields(comp->fields);
    for (const auto* info : header.structs) {
        name_struct(info->qualified_name);
        name_fields(info->fields);
    }
    for (const auto& q : named) out << "extern \"C\" const FjellTypeDesc " << type_symbol(q) << ";\n";
    if (!named.empty()) out << "\n";

    bool first = true;
    for (const auto* comp : header.components) {
        if (!first) out << "\n";
        first = false;
        emit_component(out, *comp);
    }
    for (const auto* info : header.structs) {
        if (!first) out << "\n";
        first = false;
        emit_struct(out, *info);
    }
    for (const auto* pipe : header.pipes) emit_pipe(out, *pipe);
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
        {"FJELL_FIELD_STRUCT", "struct"},     {"FJELL_FIELD_VECTOR", "list"},
    };
    for (const auto& [name, kind] : kinds) {
        if (f.fj_field_type == name) return kind;
    }
    return "unknown";
}

const char* element_kind(const FieldInfo& f) {
    FieldInfo element;
    element.fj_field_type = f.element_fj_type;
    return field_kind(element);
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

using Json = nlohmann::ordered_json;

Json field_json(const FieldInfo& f) {
    Json field;
    field["name"] = f.name;
    field["type"] = f.cpp_type;
    field["kind"] = field_kind(f);
    field["line"] = f.line;
    if (f.is_state) field["state"] = true;
    if (f.is_getter) field["getter"] = true;
    if (f.is_model) field["model"] = true;
    if (f.fj_field_type == "FJELL_FIELD_STRUCT") field["struct"] = f.struct_type;
    if (f.fj_field_type == "FJELL_FIELD_VECTOR") {
        field["element"] = element_kind(f);
        if (f.element_fj_type == "FJELL_FIELD_STRUCT") field["element_struct"] = f.element_struct;
    }
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
    return field;
}

/// Everything fjreflect read, for tools that check a project against its
/// scripts (the lint, the editor). Nothing in the build reads it.
std::string manifest(const std::vector<ComponentInfo>& components, const std::vector<StructInfo>& structs,
                     const std::vector<PipeInfo>& pipes) {
    Json types = Json::array();
    for (const auto& comp : components) {
        Json type;
        type["name"] = comp.class_name;
        type["qualified_name"] = comp.qualified_name;
        type["kind"] = comp.is_ui_component ? "ui_widget" : comp.is_notify ? "notify" : "component";
        type["header"] = std::filesystem::path(comp.source_file).filename().string();
        Json fields = Json::array();
        for (const auto& f : comp.fields) fields.push_back(field_json(f));
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
    Json structs_json = Json::array();
    for (const auto& info : structs) {
        Json fields = Json::array();
        for (const auto& f : info.fields) fields.push_back(field_json(f));
        structs_json.push_back({{"name", info.name},
                                {"qualified_name", info.qualified_name},
                                {"header", std::filesystem::path(info.source_file).filename().string()},
                                {"line", info.line},
                                {"fields", std::move(fields)}});
    }
    Json pipes_json = Json::array();
    for (const auto& pipe : pipes) {
        FieldInfo input;
        input.fj_field_type = pipe.input_fj_type;
        Json args = Json::array();
        for (const auto& kind : pipe.arg_fj_types) {
            FieldInfo arg;
            arg.fj_field_type = kind;
            args.push_back(field_kind(arg));
        }
        pipes_json.push_back({{"name", pipe.name},
                              {"input", field_kind(input)},
                              {"args", std::move(args)},
                              {"header", std::filesystem::path(pipe.source_file).filename().string()},
                              {"line", pipe.line}});
    }
    Json root;
    root["version"] = 2;
    root["types"] = std::move(types);
    root["structs"] = std::move(structs_json);
    root["pipes"] = std::move(pipes_json);
    return root.dump(2) + "\n";
}

} // namespace

std::string register_file(const std::vector<ComponentInfo>& components, const std::vector<StructInfo>& structs,
                          const std::vector<PipeInfo>& pipes) {
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

    if (!structs.empty()) {
        out << "\n";
        for (const auto& info : structs) {
            out << "extern \"C\" const FjellTypeDesc " << type_symbol(info.qualified_name) << ";\n";
        }
        out << "static const FjellTypeDesc* const fjreflect_types[] = {\n";
        for (const auto& info : structs) out << "    &" << type_symbol(info.qualified_name) << ",\n";
        out << "};\n";
    }
    if (!pipes.empty()) {
        out << "\n";
        for (const auto& pipe : pipes) {
            out << "extern \"C\" void fjreflect_pipe_" << pipe.name << "(const void*, const void* const*, void*);\n";
            if (pipe.arg_fj_types.empty()) continue;
            out << "static const FjellFieldType fjreflect_pipe_" << pipe.name << "_args[] = {";
            for (std::size_t i = 0; i < pipe.arg_fj_types.size(); ++i) {
                out << (i ? ", " : "") << pipe.arg_fj_types[i];
            }
            out << "};\n";
        }
        out << "static const FjellPipeDesc fjreflect_pipes[] = {\n";
        for (const auto& pipe : pipes) {
            out << "    {\"" << pipe.name << "\", " << pipe.input_fj_type << ", ";
            if (pipe.arg_fj_types.empty()) out << "nullptr, 0, ";
            else out << "fjreflect_pipe_" << pipe.name << "_args, " << pipe.arg_fj_types.size() << ", ";
            out << "fjreflect_pipe_" << pipe.name << "},\n";
        }
        out << "};\n";
    }

    // FJELL_REGISTER()'s entry points, with the generated tables handed over
    // before the types register: structs and pipes first, because components
    // hold structs.
    out << "\nextern \"C\" FJELL_GAME_EXPORT uint32_t fjell_api_version() { return FJELL_API_VERSION; }\n";
    out << "FJELL_DEFINE_ENGINE_COMPONENT_LAYOUTS()\n";
    out << "extern \"C\" FJELL_GAME_EXPORT void fjell_register(FjellTypeRegistry* registry) {\n";
    if (!structs.empty()) out << "    fjell_reg_types(registry, fjreflect_types, " << structs.size() << ");\n";
    if (!pipes.empty()) out << "    fjell_reg_pipes(registry, fjreflect_pipes, " << pipes.size() << ");\n";
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

GeneratedFiles generate(const std::vector<ComponentInfo>& components, const std::vector<StructInfo>& structs,
                        const std::vector<PipeInfo>& pipes) {
    // What each header contributes, headers in the order they first appear.
    std::vector<HeaderOutput> headers;
    auto header_for = [&](const std::string& source_file) -> HeaderOutput& {
        auto it = std::find_if(headers.begin(), headers.end(),
                               [&](const HeaderOutput& h) { return h.source_file == source_file; });
        if (it != headers.end()) return *it;
        HeaderOutput& header = headers.emplace_back();
        header.source_file = source_file;
        return header;
    };
    for (const auto& comp : components) header_for(comp.source_file).components.push_back(&comp);
    for (const auto& info : structs) header_for(info.source_file).structs.push_back(&info);
    for (const auto& pipe : pipes) header_for(pipe.source_file).pipes.push_back(&pipe);

    GeneratedFiles files;
    for (const auto& header : headers) {
        const auto stem = stem_of(header.source_file);
        files[stem + ".generated.cpp"] = generated_file(stem, header);
    }
    files["manifest.json"] = manifest(components, structs, pipes);
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
