#include "scripting/reflected_types.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <ranges>

namespace fjell {

namespace {

/// The Types a [[fj::shader_type]] names. A word naming none is dropped and
/// reported against the field; the rest still narrow the slot.
std::vector<ShaderType> parse_shader_types(const char* names, const char* owner, const char* field_name) {
    std::vector<ShaderType> types;
    if (names == nullptr) return types;
    for (auto part : std::string_view(names) | std::views::split(',')) {
        std::string_view word(part.begin(), part.end());
        if (auto type = shader_type_from_name(word)) {
            types.push_back(*type);
        } else {
            FJELL_CORE_ERROR("Script '{}': [[fj::shader_type]] on '{}' names '{}', which is not "
                             "a shader type (spatial, sky, ui, terrain, water, fog)",
                             owner ? owner : "<unnamed>", field_name, word);
        }
    }
    return types;
}

} // namespace

const OwnedFieldDesc* OwnedTypeDesc::find_field(std::string_view field) const {
    auto it = std::find_if(fields.begin(), fields.end(), [&](const OwnedFieldDesc& f) { return f.name == field; });
    return it == fields.end() ? nullptr : &*it;
}

void ReflectedTypes::clear() {
    types_.clear();
    by_desc_.clear();
    pipes_.clear();
}

void ReflectedTypes::add_types(const FjellTypeDesc* const* types, uint32_t count) {
    if (types == nullptr) return;
    // Every struct first, then their fields, which may name any of them.
    const std::size_t first = types_.size();
    for (uint32_t i = 0; i < count; ++i) {
        const FjellTypeDesc* desc = types[i];
        if (desc == nullptr || desc->name == nullptr) continue;
        OwnedTypeDesc& owned = types_.emplace_back();
        owned.name = desc->name;
        owned.qualified_name = desc->qualified_name ? desc->qualified_name : desc->name;
        owned.size = desc->size;
        owned.align = desc->align;
        owned.construct = desc->construct;
        owned.destruct = desc->destruct;
        by_desc_[desc] = &owned;
    }
    std::size_t index = first;
    for (uint32_t i = 0; i < count; ++i) {
        const FjellTypeDesc* desc = types[i];
        if (desc == nullptr || desc->name == nullptr) continue;
        copy_fields(desc->fields, desc->field_count, desc->name, types_[index++].fields);
    }
}

void ReflectedTypes::add_pipes(const FjellPipeDesc* pipes, uint32_t count) {
    if (pipes == nullptr) return;
    for (uint32_t i = 0; i < count; ++i) {
        const FjellPipeDesc& desc = pipes[i];
        if (desc.name == nullptr || desc.call == nullptr) continue;
        OwnedPipeDesc owned;
        owned.name = desc.name;
        owned.input = desc.input;
        if (desc.arg_types != nullptr) owned.arg_types.assign(desc.arg_types, desc.arg_types + desc.arg_count);
        owned.call = desc.call;
        pipes_.push_back(std::move(owned));
    }
}

const OwnedTypeDesc* ReflectedTypes::find(std::string_view name) const {
    auto it = std::find_if(types_.begin(), types_.end(), [&](const OwnedTypeDesc& t) {
        return t.name == name || t.qualified_name == name;
    });
    return it == types_.end() ? nullptr : &*it;
}

const OwnedPipeDesc* ReflectedTypes::find_pipe(std::string_view name) const {
    auto it = std::find_if(pipes_.begin(), pipes_.end(), [&](const OwnedPipeDesc& p) { return p.name == name; });
    return it == pipes_.end() ? nullptr : &*it;
}

const OwnedTypeDesc* ReflectedTypes::resolve(const FjellTypeDesc* desc) const {
    if (desc == nullptr) return nullptr;
    auto it = by_desc_.find(desc);
    return it == by_desc_.end() ? nullptr : it->second;
}

OwnedFieldDesc ReflectedTypes::copy_field(const FjellFieldDesc& fd, const char* owner) const {
    OwnedFieldDesc ofd;
    ofd.name = fd.name;
    ofd.type = fd.type;
    ofd.offset = fd.offset;
    ofd.size = fd.size;
    ofd.min = fd.min;
    ofd.max = fd.max;
    ofd.speed = fd.speed;
    ofd.readonly = fd.readonly != 0;
    ofd.replicated = fd.replicated != 0;
    if (fd.ref_type_name) {
        ofd.ref_type_name = fd.ref_type_name;
    }
    if (fd.tooltip) {
        ofd.tooltip = fd.tooltip;
    }
    if (fd.category) {
        ofd.category = fd.category;
    }
    ofd.shader_types = parse_shader_types(fd.shader_types, owner, fd.name);
    if (fd.type == FJELL_FIELD_ENUM && fd.enum_labels && fd.size > 0) {
        for (uint32_t e = 0; e < fd.size; ++e) {
            if (fd.enum_labels[e]) {
                ofd.enum_labels.emplace_back(fd.enum_labels[e]);
            }
        }
    }
    ofd.flags = fd.flags;
    ofd.get = fd.get;
    if (fd.type == FJELL_FIELD_STRUCT) {
        ofd.struct_type = resolve(fd.type_desc);
        if (ofd.struct_type == nullptr) {
            FJELL_CORE_ERROR("Script '{}': field '{}' holds a struct the module did not register; "
                             "rebuild scripts from the editor", owner ? owner : "<unnamed>", fd.name);
        }
    }
    if (fd.type == FJELL_FIELD_VECTOR && fd.vector != nullptr) {
        ofd.vector.element = fd.vector->element;
        ofd.vector.element_type = resolve(fd.vector->element_type);
        ofd.vector.element_size = fd.vector->element_size;
        ofd.vector.size = fd.vector->size;
        ofd.vector.at = fd.vector->at;
        ofd.vector.insert = fd.vector->insert;
        ofd.vector.erase = fd.vector->erase;
        ofd.vector.move = fd.vector->move;
    }
    return ofd;
}

void ReflectedTypes::copy_fields(const FjellFieldDesc* fields, uint32_t count, const char* owner,
                                 std::vector<OwnedFieldDesc>& out) const {
    for (uint32_t i = 0; i < count; ++i) {
        if (fields == nullptr || fields[i].name == nullptr) {
            FJELL_CORE_ERROR("Script '{}' declares {} reflected fields but its table ends at "
                             "{} — generated code is stale. Rebuild scripts from the editor.",
                             owner ? owner : "<unnamed>", count, i);
            return;
        }
        out.push_back(copy_field(fields[i], owner));
    }
}

} // namespace fjell
