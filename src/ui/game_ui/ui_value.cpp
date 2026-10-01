#include "ui/game_ui/ui_value.hpp"
#include "scripting/script_call.hpp"

#include "scripting/fjell/ref.hpp"
#include "scripting/reflected_types.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <string>

namespace fjell {

namespace {

std::string float_text(float value) {
    char buf[32];
    auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), value);
    return ec == std::errc{} ? std::string(buf, end) : std::string("0");
}

/// A getter's result. `get` writes the type the getter returns.
template <class T>
UiValue call_getter(const OwnedFieldDesc& field, const void* object) {
    T value{};
    // A getter is game code: through call_script(), so what it throws is
    // reported and reads as the default.
    const ScriptCall call{.type_name = field.name, .instance = object};
    call_script(call, [&] { field.get(object, &value); });
    return UiValue(std::move(value));
}

/// Where `path` leads from `address` now: null where there is nothing to
/// read, at an entry its vector no longer has among them.
const void* follow(const void* address, const UiPath& path) {
    const char* at = static_cast<const char*>(address);
    for (const UiStep& step : path) {
        if (at == nullptr) return nullptr;
        if (step.vector == nullptr) {
            at += step.value;
        } else {
            if (step.value >= step.vector->size(at)) return nullptr;
            at = static_cast<const char*>(step.vector->at(at, step.value));
        }
    }
    return at;
}

/// A place: an address and the path from it.
struct Place {
    const void* address;
    UiPath path;
};

/// `offset` bytes into what (`address`, `path`) reaches. Before any entry
/// the address does not move, so the offset goes into it and a struct
/// inside a Model keeps an empty path.
Place offset_into(const void* address, const UiPath& path, uint32_t offset) {
    if (path.empty()) return {static_cast<const char*>(address) + offset, {}};
    UiPath further = path;
    if (further.back().vector == nullptr) {
        further.back().value += offset;
    } else {
        further.push_back({nullptr, offset});
    }
    return {address, std::move(further)};
}

/// A value as it is stored at `at`, which `place` leads to: a field of an
/// object, or a list's entry. An object or a list is handed out as its
/// place, so it is read where it is each time. `size` is a char buffer's
/// capacity, `labels` an enum's names.
UiValue read_stored(FjellFieldType type, const void* at, const Place& place, uint32_t size,
                    std::span<const std::string> labels, const OwnedTypeDesc* struct_type,
                    const OwnedVectorDesc* vector) {
    switch (type) {
        case FJELL_FIELD_BOOL: return UiValue(*static_cast<const bool*>(at));
        case FJELL_FIELD_INT: return UiValue(*static_cast<const int32_t*>(at));
        case FJELL_FIELD_FLOAT: return UiValue(*static_cast<const float*>(at));
        case FJELL_FIELD_VEC2: return UiValue(*static_cast<const glm::vec2*>(at));
        case FJELL_FIELD_VEC3:
        case FJELL_FIELD_COLOR3: return UiValue(*static_cast<const glm::vec3*>(at));
        case FJELL_FIELD_VEC4:
        case FJELL_FIELD_COLOR4: return UiValue(*static_cast<const glm::vec4*>(at));
        case FJELL_FIELD_STD_STRING: return UiValue(*static_cast<const std::string*>(at));
        case FJELL_FIELD_STRING: {
            std::string_view chars(static_cast<const char*>(at), size);
            return UiValue(std::string(chars.substr(0, chars.find('\0'))));
        }
        case FJELL_FIELD_ENUM: {
            // Shown by its label; a value past the labels by its number.
            const int32_t index = *static_cast<const int32_t*>(at);
            if (index >= 0 && static_cast<size_t>(index) < labels.size()) {
                return UiValue(labels[static_cast<size_t>(index)]);
            }
            return UiValue(index);
        }
        case FJELL_FIELD_STRUCT:
            if (struct_type == nullptr) return {};
            return UiValue(UiObject{.type = struct_type->name,
                                    .fields = struct_type->fields,
                                    .address = place.address,
                                    .path = place.path});
        case FJELL_FIELD_VECTOR:
            if (vector == nullptr || vector->size == nullptr || vector->at == nullptr) return {};
            return UiValue(UiList{.vector = vector, .address = place.address, .path = place.path});
        default: return {}; // references and widget pointers are not values markup reads
    }
}

/// What a field of `object`, which is at `at` now, holds.
UiValue read_field(const OwnedFieldDesc& field, const UiObject& object, const void* at) {
    if (field.is_getter()) {
        if (field.get == nullptr) return {};
        switch (field.type) {
            case FJELL_FIELD_BOOL: return call_getter<bool>(field, at);
            case FJELL_FIELD_INT: return call_getter<int32_t>(field, at);
            case FJELL_FIELD_FLOAT: return call_getter<float>(field, at);
            case FJELL_FIELD_VEC2: return call_getter<glm::vec2>(field, at);
            case FJELL_FIELD_VEC3: return call_getter<glm::vec3>(field, at);
            case FJELL_FIELD_VEC4: return call_getter<glm::vec4>(field, at);
            case FJELL_FIELD_STD_STRING: return call_getter<std::string>(field, at);
            default: return {}; // fjreflect accepts no other return type
        }
    }
    return read_stored(field.type, static_cast<const char*>(at) + field.offset,
                       offset_into(object.address, object.path, field.offset), field.size, field.enum_labels,
                       field.struct_type, &field.vector);
}

/// `x y z w`, or `r g b a`: a component's index, or -1.
int component_index(std::string_view name) {
    if (name == "x" || name == "r") return 0;
    if (name == "y" || name == "g") return 1;
    if (name == "z" || name == "b") return 2;
    if (name == "w" || name == "a") return 3;
    return -1;
}

std::optional<float> read_float(const std::string& text) {
    try {
        return std::stof(text);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<int> read_int(const std::string& text) {
    try {
        return std::stoi(text);
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace

bool write_value(FjellFieldType kind, void* at, const UiValue& value) {
    switch (kind) {
        case FJELL_FIELD_BOOL: *static_cast<bool*>(at) = value.truthy(); return true;
        case FJELL_FIELD_INT:
        case FJELL_FIELD_FLOAT: {
            const auto n = value.number();
            if (!n) return false;
            if (kind == FJELL_FIELD_INT) *static_cast<int32_t*>(at) = static_cast<int32_t>(std::trunc(*n));
            else *static_cast<float*>(at) = *n;
            return true;
        }
        case FJELL_FIELD_STD_STRING: *static_cast<std::string*>(at) = value.text(); return true;
        default: return false;
    }
}

bool write_ref(void* at, std::string_view type, const UiValue& value) {
    auto& ref = *static_cast<RefData*>(at);
    uint32_t node = 0;
    uint32_t generation = 0;
    if (const UiObject* object = value.object()) {
        if (object->type != type || object->node == 0) return false;
        node = object->node;
        generation = object->generation;
    } else if (!value.empty()) {
        return false;
    }
    if (ref.node_id != node || ref.generation != generation) ref = {.node_id = node, .generation = generation};
    return true;
}

UiValue UiValue::parse(std::string_view text, std::string_view type) {
    const std::string s(text);
    if (type == "float") {
        if (auto f = read_float(s)) return UiValue(*f);
    } else if (type == "int") {
        if (auto i = read_int(s)) return UiValue(*i);
    } else if (type == "bool") {
        return UiValue(s != "0" && s != "false");
    }
    return UiValue(s);
}

UiValue UiValue::as(std::string_view type) const {
    if (type == "float") {
        if (auto f = number()) return UiValue(*f);
    } else if (type == "int") {
        if (auto f = number()) return UiValue(static_cast<int>(std::trunc(*f)));
    } else if (type == "bool") {
        return UiValue(truthy());
    } else if (type == "string") {
        return UiValue(text());
    }
    return *this;
}

std::string UiValue::text() const {
    if (const auto* b = std::get_if<bool>(&value_)) return *b ? "1" : "0";
    if (const auto* i = std::get_if<int>(&value_)) return std::to_string(*i);
    if (const auto* f = std::get_if<float>(&value_)) return float_text(*f);
    if (const auto* s = std::get_if<std::string>(&value_)) return *s;
    if (const auto* v = std::get_if<UiVector>(&value_)) {
        std::string out = float_text(v->value[0]);
        for (int i = 1; i < v->length; ++i) out += ", " + float_text(v->value[i]);
        return out;
    }
    return {};
}

std::optional<float> UiValue::number() const {
    if (const auto* b = std::get_if<bool>(&value_)) return *b ? 1.0f : 0.0f;
    if (const auto* i = std::get_if<int>(&value_)) return static_cast<float>(*i);
    if (const auto* f = std::get_if<float>(&value_)) return *f;
    if (const auto* s = std::get_if<std::string>(&value_)) return read_float(*s);
    return std::nullopt;
}

bool UiValue::truthy() const {
    if (const auto* b = std::get_if<bool>(&value_)) return *b;
    if (const auto* i = std::get_if<int>(&value_)) return *i != 0;
    if (const auto* f = std::get_if<float>(&value_)) return *f != 0.0f;
    if (const auto* s = std::get_if<std::string>(&value_)) return !s->empty() && *s != "0" && *s != "false";
    if (const auto* v = std::get_if<UiVector>(&value_)) return v->value != glm::vec4(0.0f);
    if (const auto* o = std::get_if<UiObject>(&value_)) return o->where() != nullptr;
    if (const auto* l = std::get_if<UiList>(&value_)) return l->size() > 0;
    return false;
}

const void* UiObject::where() const {
    return follow(address, path);
}

const void* UiList::where() const {
    return follow(address, path);
}

std::size_t UiList::size() const {
    const void* list = where();
    return vector != nullptr && list != nullptr ? vector->size(list) : 0;
}

UiValue UiList::at(std::size_t index) const {
    Place entry{address, path};
    entry.path.push_back({vector, static_cast<uint32_t>(index)});
    const void* stored = follow(entry.address, entry.path);
    if (stored == nullptr) return {};
    return read_stored(vector->element, stored, entry, vector->element_size, {}, vector->element_type, nullptr);
}

UiValue UiValue::member(std::string_view name) const {
    if (const auto* object = std::get_if<UiObject>(&value_)) {
        const void* at = object->where();
        if (at == nullptr) return {};
        for (const auto& field : object->fields) {
            if (field.name == name) return read_field(field, *object, at);
        }
        return {};
    }
    if (const auto* vector = std::get_if<UiVector>(&value_)) {
        const int index = component_index(name);
        if (index < 0 || index >= vector->length) return {};
        return UiValue(vector->value[index]);
    }
    return {};
}

} // namespace fjell
