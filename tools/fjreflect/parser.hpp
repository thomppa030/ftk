#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fjreflect {

struct FieldInfo {
    std::string name;
    std::string cpp_type;      // "float", "fjell::WeakRef<Node3D>", etc.
    std::string fj_field_type; // "FJELL_FIELD_FLOAT", etc.
    std::string ref_target;    // for WeakRef<X>/Ref<X>: "X", else empty
    uint32_t offset{0};        // filled later by offset query
    uint32_t size{0};          // filled later by offset query
    unsigned line{0};          // source line number

    // Hints
    float hint_min{0.0f};
    float hint_max{0.0f};
    float hint_speed{0.0f};
    bool has_range{false};
};

struct ComponentInfo {
    std::string class_name;
    std::string source_file;
    std::vector<FieldInfo> fields;
};

struct ParseResult {
    std::vector<ComponentInfo> components;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

/// Text-based parse: find FJELL_COMPONENT classes and [[fj::property]] fields.
ParseResult parse_file(const std::string& filepath);

/// Compile and run a tiny program to query offsetof/sizeof for all fields.
/// Fills in offset and size on each FieldInfo. Returns false on failure.
bool query_offsets(std::vector<ComponentInfo>& components,
                   const std::string& source_file,
                   const std::vector<std::string>& include_dirs,
                   const std::string& compiler);

} // namespace fjreflect
