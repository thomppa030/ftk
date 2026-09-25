#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace fjell {

/// One value in a UI scope: a prop, a controller's published field, a list
/// item's field. It keeps the type it was given until something shows it, and
/// markup reads it three ways: as text (`{key}`), as a number (a bar's value)
/// and as true or false (`if=`, `class:`).
class UiValue {
public:
    UiValue() = default; ///< no value: empty text, false, no number
    UiValue(std::string text) : value_(std::move(text)) {} // NOLINT(google-explicit-constructor): text is the common case
    UiValue(const char* text) : value_(std::string(text)) {} // NOLINT(google-explicit-constructor)
    explicit UiValue(bool value) : value_(value) {}
    explicit UiValue(int value) : value_(value) {}
    explicit UiValue(float value) : value_(value) {}

    /// Text as a declared prop type reads it: `float`, `int`, `bool` or
    /// `string`. Text that is not a number of that type stays text, as the
    /// author wrote it; an unknown type keeps the text too.
    [[nodiscard]] static UiValue parse(std::string_view text, std::string_view type);

    /// This value as a declared prop type, for a prop bound from another
    /// value. A value that cannot be one (text that is not a number) is kept
    /// as it is.
    [[nodiscard]] UiValue as(std::string_view type) const;

    [[nodiscard]] bool empty() const { return std::holds_alternative<std::monostate>(value_); }

    /// As markup shows it: `1`/`0` for a bool, the shortest decimal that reads
    /// back as the same float.
    [[nodiscard]] std::string text() const;

    /// As a number: text is read as one if it starts with one. None for no
    /// value, or text that is not a number.
    [[nodiscard]] std::optional<float> number() const;

    /// For `if=` and `class:`: false for no value, false, zero, and the texts
    /// "", "0" and "false".
    [[nodiscard]] bool truthy() const;

    /// The same type holding the same value.
    bool operator==(const UiValue&) const = default;

private:
    std::variant<std::monostate, bool, int, float, std::string> value_;
};

} // namespace fjell
