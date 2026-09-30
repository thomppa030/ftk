#pragma once

#include "scripting/fjell/api.h"
#include "scripting/reflected_types.hpp"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace fjell {

class UiValue;

/// A reflected object a scope holds, read in place: a widget's Model, or a
/// struct inside one. It points into the object, so whoever puts it in a
/// scope keeps it current; the fields belong to the game module and go with
/// it on a reload.
struct UiObject {
    std::string_view type;                  ///< the type's name as its header spells it
    std::span<const OwnedFieldDesc> fields; ///< what a path may read
    const void* address{nullptr};           ///< null while there is nothing to read
    /// For a component, the node it is on and which life of that node, so a
    /// `Ref` can be made to it; 0 for anything else.
    uint32_t node{0};
    uint32_t generation{0};

    /// The same object: same place, read as the same type, on the same node
    /// (a slot another node's component took over is another object).
    friend bool operator==(const UiObject& a, const UiObject& b) {
        return a.address == b.address && a.fields.data() == b.fields.data() && a.node == b.node &&
               a.generation == b.generation;
    }
};

/// A list a scope holds, read in place: a `std::vector` field of a Model,
/// through the functions generated for its exact type. Like an object it
/// points into the game's data, so whoever puts it in a scope keeps it
/// current.
struct UiList {
    const OwnedVectorDesc* vector{nullptr};
    const void* address{nullptr}; ///< the std::vector itself

    [[nodiscard]] std::size_t size() const;
    /// The entry at `index`, which must be below size(): an object for a
    /// struct, the value itself for anything else.
    [[nodiscard]] UiValue at(std::size_t index) const;

    /// The same list: same place, read as the same type.
    friend bool operator==(const UiList& a, const UiList& b) {
        return a.address == b.address && a.vector == b.vector;
    }
};

/// A `glm::vec2`, `vec3` or `vec4`, or a colour.
struct UiVector {
    glm::vec4 value{0.0f};
    int length{4}; ///< 2, 3 or 4
    bool operator==(const UiVector&) const = default;
};

/// One value in a UI scope: a prop, a Model, a controller's published field,
/// a list item's field. It keeps the type it was given until something shows
/// it, and markup reads it three ways: as text (`{key}`), as a number (a
/// bar's value) and as true or false (`if=`, `class:`). An object or a vector
/// is also read into, one `.name` at a time.
class UiValue {
public:
    UiValue() = default; ///< no value: empty text, false, no number
    UiValue(std::string text) : value_(std::move(text)) {} // NOLINT(google-explicit-constructor): text is the common case
    UiValue(const char* text) : value_(std::string(text)) {} // NOLINT(google-explicit-constructor)
    explicit UiValue(bool value) : value_(value) {}
    explicit UiValue(int value) : value_(value) {}
    explicit UiValue(float value) : value_(value) {}
    explicit UiValue(glm::vec2 value) : value_(UiVector{glm::vec4(value, 0.0f, 0.0f), 2}) {}
    explicit UiValue(glm::vec3 value) : value_(UiVector{glm::vec4(value, 0.0f), 3}) {}
    explicit UiValue(glm::vec4 value) : value_(UiVector{value, 4}) {}
    explicit UiValue(UiObject object) : value_(object) {}
    explicit UiValue(UiList list) : value_(list) {}

    /// Text as a declared prop type reads it: `float`, `int`, `bool` or
    /// `string`. Text that is not a number of that type stays text, as the
    /// author wrote it; an unknown type keeps the text too.
    [[nodiscard]] static UiValue parse(std::string_view text, std::string_view type);

    /// This value as a declared prop type, for a prop bound from another
    /// value. A value that cannot be one (text that is not a number) is kept
    /// as it is.
    [[nodiscard]] UiValue as(std::string_view type) const;

    [[nodiscard]] bool empty() const { return std::holds_alternative<std::monostate>(value_); }

    /// The object this holds, or null.
    [[nodiscard]] const UiObject* object() const { return std::get_if<UiObject>(&value_); }
    /// The list this holds, or null.
    [[nodiscard]] const UiList* list() const { return std::get_if<UiList>(&value_); }
    /// The vector this holds, or null.
    [[nodiscard]] const UiVector* vector() const { return std::get_if<UiVector>(&value_); }

    /// As markup shows it: `1`/`0` for a bool, the shortest decimal that reads
    /// back as the same float, a vector's components separated by `, `. An
    /// object or a list has no text.
    [[nodiscard]] std::string text() const;

    /// As a number: text is read as one if it starts with one. None for no
    /// value, a vector, an object, or text that is not a number.
    [[nodiscard]] std::optional<float> number() const;

    /// For `if=` and `class:`: false for no value, false, zero, a vector of
    /// zeros, an object with nothing to read, an empty list, and the texts
    /// "", "0" and "false".
    [[nodiscard]] bool truthy() const;

    /// One step of a path: an object's field or getter by name, or a
    /// vector's component (`x` `y` `z` `w`, or `r` `g` `b` `a`). No value
    /// when there is no such member, when this is neither, or when the
    /// object has nothing to read. A getter runs on every call.
    [[nodiscard]] UiValue member(std::string_view name) const;

    /// The same type holding the same value; objects by identity.
    bool operator==(const UiValue&) const = default;

private:
    std::variant<std::monostate, bool, int, float, std::string, UiVector, UiObject, UiList> value_;
};

/// Writes `value` into a field of kind bool, int, float or std::string at
/// `at`, as that kind reads it: true or false as `if=` reads it, a number
/// from text that is one, text as markup shows it. False, writing nothing,
/// for another kind, or for a number the value cannot be read as.
bool write_value(FjellFieldType kind, void* at, const UiValue& value);

/// Points the `Ref<T>` at `at` at the component `value` holds, when that is a
/// component of type `type`; empties it when `value` holds nothing. A Ref
/// already pointing there is left as it is, keeping what it found. False,
/// writing nothing, for a value that is something else.
bool write_ref(void* at, std::string_view type, const UiValue& value);

} // namespace fjell
