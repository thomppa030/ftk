#pragma once

// The types the UI core and the kit above it share: a colour, a widget's
// id, a picture by handle, the corners a rectangle rounds, and what a drag
// carries. They are the kit's
// (ftk/ui/types.hpp includes them), kept here so the core has them without
// the kit. Points and sizes are glm::vec2.

#include <glm/vec4.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ftk::ui {

/// A colour: red, green, blue and alpha from 0 to 1, in sRGB like every
/// colour the theme names.
using Color = glm::vec4;

/// A widget's id: its label hashed with the ids pushed around it. While
/// ImGui draws the kit it is ImGui's own id.
using Id = uint32_t;

/// A picture the UI shows, by handle: made from a GPU texture by what draws
/// the UI, which keeps the texture behind it. Zero is none. While ImGui
/// draws the kit it holds ImGui's texture id, and is made from one without
/// a word.
struct Texture {
    uint64_t id{0};

    constexpr Texture() = default;
    constexpr Texture(uint64_t value) : id{value} {}

    constexpr explicit operator bool() const { return id != 0; }
    friend constexpr bool operator==(Texture, Texture) = default;
};

/// The corners of a rectangle that are rounded.
enum class RoundCorners : uint8_t {
    All,
    Top,
    Bottom,
    Left,
    Right,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
    None,
};

/// What a drag carries: the bytes its source gave, as it gave them.
struct Payload {
    const void* data{nullptr};
    std::size_t size{0};

    /// The bytes as text, without the zero a text payload ends with.
    [[nodiscard]] std::string_view text() const {
        const auto* chars = static_cast<const char*>(data);
        const std::size_t length = size > 0 && chars[size - 1] == '\0' ? size - 1 : size;
        return {chars, length};
    }
};

} // namespace ftk::ui
