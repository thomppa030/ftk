#pragma once

#include <string>

namespace fjreflect {

/// The field kind (`FJELL_FIELD_...`) a C++ type maps to, or empty when it
/// maps to none. For Asset<T> and Ref<T>, `ref_target` gets T's bare name.
[[nodiscard]] std::string map_field_type(const std::string& cpp_type, std::string& ref_target);

/// A raw pointer to something the engine owns (a node, a component, an
/// asset): nothing keeps it valid when that goes away.
[[nodiscard]] bool is_engine_pointer(const std::string& type);

/// What a piece of runtime state or a Model member can hold: a value kind,
/// a reference, a struct (by name, resolved by the linker) or a std::vector
/// of either. `error` says why a type cannot be held; everything else is
/// empty then.
struct ValueType {
    std::string fj;             // the kind
    std::string ref_target;     // references: the target's bare name
    std::string struct_name;    // STRUCT: as written
    std::string element_cpp;    // VECTOR: the element type as written
    std::string element_fj;     // VECTOR: the element's kind
    std::string element_struct; // VECTOR of structs: as written
    std::string error;
};
[[nodiscard]] ValueType classify_value(const std::string& cpp_type);

/// A kind a getter may return and a pipe may take: a plain value.
[[nodiscard]] bool is_plain_value(const std::string& fj);

/// The type with a leading `const` and trailing references removed: how a
/// value is passed, not what it is.
[[nodiscard]] std::string value_type_of(const std::string& type);

} // namespace fjreflect
