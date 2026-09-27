#include "types.hpp"

#include <cctype>
#include <utility>

namespace fjreflect {

std::string map_field_type(const std::string& cpp_type, std::string& ref_target) {
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
    if (bare == "std::string") return "FJELL_FIELD_STD_STRING";

    // WeakRef<T>, Asset<T>, Ref<T>, SharedRef<T>: the target's bare name
    static const std::pair<const char*, const char*> refs[] = {
        {"WeakRef<", "FJELL_FIELD_WEAK_REF"},
        {"Asset<", "FJELL_FIELD_ASSET"},
        {"Ref<", "FJELL_FIELD_REF"},
        {"SharedRef<", "FJELL_FIELD_SHARED_REF"},
    };
    for (const auto& [prefix, kind] : refs) {
        if (!bare.starts_with(prefix)) continue;
        auto start = bare.find('<') + 1;
        auto end = bare.rfind('>');
        ref_target = bare.substr(start, end - start);
        if (auto pos = ref_target.rfind("::"); pos != std::string::npos)
            ref_target = ref_target.substr(pos + 2);
        return kind;
    }

    return {}; // Unknown
}

bool is_engine_pointer(const std::string& type) {
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

namespace {

/// Names a class: words and `::` only, no template arguments or pointers.
bool is_plain_name(const std::string& type) {
    if (type.empty()) return false;
    for (char c : type) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != ':') return false;
    }
    return true;
}

ValueType cannot_hold(std::string why) {
    ValueType v;
    v.error = std::move(why);
    return v;
}

} // namespace

ValueType classify_value(const std::string& cpp_type) {
    ValueType v;
    v.fj = map_field_type(cpp_type, v.ref_target);
    if (!v.fj.empty()) return v;

    if (cpp_type.starts_with("std::vector<") && cpp_type.ends_with(">")) {
        v.fj = "FJELL_FIELD_VECTOR";
        v.element_cpp = cpp_type.substr(12, cpp_type.size() - 13);
        std::string element_ref;
        v.element_fj = map_field_type(v.element_cpp, element_ref);
        if (v.element_fj == "FJELL_FIELD_BOOL") {
            return cannot_hold("std::vector<bool> keeps its values as bits, which cannot be read one "
                             "by one; use std::vector<int>");
        }
        if (!element_ref.empty()) return cannot_hold("a list cannot hold references");
        if (v.element_fj.empty()) {
            if (!is_plain_name(v.element_cpp)) {
                return cannot_hold("a list holds values or structs, not '" + v.element_cpp + "'");
            }
            v.element_fj = "FJELL_FIELD_STRUCT";
            v.element_struct = v.element_cpp;
        }
        return v;
    }
    if (is_plain_name(cpp_type)) {
        v.fj = "FJELL_FIELD_STRUCT";
        v.struct_name = cpp_type;
        return v;
    }
    return cannot_hold("fjreflect has no field kind for '" + cpp_type + "'");
}

bool is_plain_value(const std::string& fj) {
    return fj == "FJELL_FIELD_FLOAT" || fj == "FJELL_FIELD_INT" || fj == "FJELL_FIELD_BOOL" ||
           fj == "FJELL_FIELD_VEC2" || fj == "FJELL_FIELD_VEC3" || fj == "FJELL_FIELD_VEC4" ||
           fj == "FJELL_FIELD_STD_STRING";
}

std::string value_type_of(const std::string& type) {
    std::string t = type;
    if (t.starts_with("const ")) t = t.substr(6);
    while (!t.empty() && (t.back() == '&' || t.back() == ' ')) t.pop_back();
    return t;
}

} // namespace fjreflect
