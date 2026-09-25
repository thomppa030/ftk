#include "ui/game_ui/ui_value.hpp"

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
    field.get(object, &value);
    return UiValue(std::move(value));
}

/// What an object's field holds now.
UiValue read_field(const OwnedFieldDesc& field, const void* object) {
    if (field.is_getter()) {
        if (field.get == nullptr) return {};
        switch (field.type) {
            case FJELL_FIELD_BOOL: return call_getter<bool>(field, object);
            case FJELL_FIELD_INT: return call_getter<int32_t>(field, object);
            case FJELL_FIELD_FLOAT: return call_getter<float>(field, object);
            case FJELL_FIELD_VEC2: return call_getter<glm::vec2>(field, object);
            case FJELL_FIELD_VEC3: return call_getter<glm::vec3>(field, object);
            case FJELL_FIELD_VEC4: return call_getter<glm::vec4>(field, object);
            case FJELL_FIELD_STD_STRING: return call_getter<std::string>(field, object);
            default: return {}; // fjreflect accepts no other return type
        }
    }
    const void* at = static_cast<const char*>(object) + field.offset;
    switch (field.type) {
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
            std::string_view chars(static_cast<const char*>(at), field.size);
            return UiValue(std::string(chars.substr(0, chars.find('\0'))));
        }
        case FJELL_FIELD_ENUM: {
            // Shown by its label; a value past the labels by its number.
            const int32_t index = *static_cast<const int32_t*>(at);
            if (index >= 0 && static_cast<size_t>(index) < field.enum_labels.size()) {
                return UiValue(field.enum_labels[static_cast<size_t>(index)]);
            }
            return UiValue(index);
        }
        case FJELL_FIELD_STRUCT:
            if (field.struct_type == nullptr) return {};
            return UiValue(UiObject{field.struct_type->name, field.struct_type->fields, at});
        default: return {}; // lists, references and widget pointers are not one value
    }
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
    if (const auto* o = std::get_if<UiObject>(&value_)) return o->address != nullptr;
    return false;
}

UiValue UiValue::member(std::string_view name) const {
    if (const auto* object = std::get_if<UiObject>(&value_)) {
        if (object->address == nullptr) return {};
        for (const auto& field : object->fields) {
            if (field.name == name) return read_field(field, object->address);
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
