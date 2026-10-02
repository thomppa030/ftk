#pragma once

// The kit's own types where its headers took ImGui's, so a program reaches
// the kit without ImGui. Points and sizes are glm::vec2.

#include <glm/vec4.hpp>

#include <cstddef>
#include <cstdint>

// ImGui's font, which a Font is while ImGui draws the kit. Named here, not
// included: the bridge goes with ImGui.
struct ImFont;

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

/// One of the theme's faces, at no size of its own: the size is given where
/// it is drawn. While ImGui draws the kit it is one of ImGui's fonts, and
/// goes wherever ImGui takes one.
class Font {
public:
    constexpr Font() = default;
    constexpr Font(ImFont* font) : font_{font} {}

    constexpr operator ImFont*() const { return font_; }

private:
    ImFont* font_{nullptr};
};

/// What a drag carries: the bytes its source gave, as it gave them.
struct Payload {
    const void* data{nullptr};
    std::size_t size{0};
};

} // namespace ftk::ui
