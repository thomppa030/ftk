#pragma once

#include "ftk/ui/kit/edit.hpp"

#include <cstddef>
#include <span>
#include <vector>

// Picking one of a fixed set of values: a body type, a blend mode, a
// collider shape. Each enum has one name table, written once next to the
// inspector that shows it, and every field for that enum reads it:
//
//     inline constexpr ui::Choice<BodyType> BODY_TYPES[] = {
//         {BodyType::Static, "Static"},
//         {BodyType::Kinematic, "Kinematic"},
//         {BodyType::Dynamic, "Dynamic"},
//     };
//     edit |= ui::choice("##body", rb.body_type, BODY_TYPES);
//
// Picking something is its own commit.

namespace ftk::ui {

/// One value of an enum and the name it is shown under.
template <typename E>
struct Choice {
    E value;
    const char* name;
};

namespace detail {
Edit combo(const char* id, int& index, std::span<const char* const> names);
Edit segmented(const char* id, int& index, std::span<const char* const> names);
[[nodiscard]] bool fits_segmented(std::span<const char* const> names);

/// Runs `field` over the names of `choices` with `value` as an index, and
/// writes the picked value back.
template <typename E, typename Field>
Edit pick(E& value, std::span<const Choice<E>> choices, Field&& field) {
    std::vector<const char*> names;
    names.reserve(choices.size());
    int index = -1;
    for (std::size_t i = 0; i < choices.size(); ++i) {
        names.push_back(choices[i].name);
        if (choices[i].value == value) index = static_cast<int>(i);
    }
    const Edit edit = field(index, std::span<const char* const>(names));
    if (edit.changed && index >= 0) value = choices[static_cast<std::size_t>(index)].value;
    return edit;
}
} // namespace detail

/// A drop-down: one sunken field with the chevron inside, the list opening
/// under it. A value missing from the table shows as blank.
template <typename E>
Edit combo(const char* id, E& value, std::span<const Choice<E>> choices) {
    return detail::pick(value, choices, [&](int& index, std::span<const char* const> names) {
        return detail::combo(id, index, names);
    });
}

/// Every option side by side, the chosen one in the toggle colour. For
/// three or fewer short options, where seeing them all beats opening a list.
template <typename E>
Edit segmented(const char* id, E& value, std::span<const Choice<E>> choices) {
    return detail::pick(value, choices, [&](int& index, std::span<const char* const> names) {
        return detail::segmented(id, index, names);
    });
}

/// The field the rule asks for: segmented when there are three options or
/// fewer and their names fit the width, a drop-down otherwise.
template <typename E>
Edit choice(const char* id, E& value, std::span<const Choice<E>> choices) {
    return detail::pick(value, choices, [&](int& index, std::span<const char* const> names) {
        return detail::fits_segmented(names) ? detail::segmented(id, index, names)
                                             : detail::combo(id, index, names);
    });
}

/// The same field for a value that is an index into names read from data
/// rather than an enum type, such as a script's enum property.
inline Edit choice(const char* id, int& index, std::span<const char* const> names) {
    return detail::fits_segmented(names) ? detail::segmented(id, index, names)
                                         : detail::combo(id, index, names);
}

// Name tables are arrays; these take them without spelling out a span.
template <typename E, std::size_t N>
Edit combo(const char* id, E& value, const Choice<E> (&choices)[N]) {
    return combo(id, value, std::span<const Choice<E>>(choices));
}
template <typename E, std::size_t N>
Edit segmented(const char* id, E& value, const Choice<E> (&choices)[N]) {
    return segmented(id, value, std::span<const Choice<E>>(choices));
}
template <typename E, std::size_t N>
Edit choice(const char* id, E& value, const Choice<E> (&choices)[N]) {
    return choice(id, value, std::span<const Choice<E>>(choices));
}

} // namespace ftk::ui
