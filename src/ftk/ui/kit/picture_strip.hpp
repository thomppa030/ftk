#pragma once

// One picked from several by its picture (sheet 34, S1), as a Texture tab
// holding several textures shows one at a time: small pictures in a row,
// each over its name, the one shown outlined in the accent, scrolling
// sideways when they don't fit.
//
//     const ui::StripPicture pictures[] = {{moss, "moss"}, {bark, "bark"}};
//     if (const auto picked = ui::picture_strip("##textures", pictures, shown)) shown = *picked;

#include "ftk/ui/types.hpp"

#include <cstddef>
#include <optional>
#include <span>

namespace ftk::ui {

/// A picture in the strip, and the name under it. Without a picture its
/// `icon` stands in, such as the asset kind's while a picture is made.
struct StripPicture {
    Texture picture{};
    const char* name{""};
    const char* icon{nullptr};
    /// What hovering it says, such as where the file is.
    const char* tooltip{nullptr};
};

/// The side of a picture in the strip.
inline constexpr float STRIP_PICTURE = 44.0f;

/// The strip across the space left, `pictures` in order, `shown` outlined.
/// @return the one clicked, if one was
std::optional<std::size_t> picture_strip(const char* id, std::span<const StripPicture> pictures, std::size_t shown);

/// How tall picture_strip() draws, for the space above it.
[[nodiscard]] float picture_strip_height();

} // namespace ftk::ui
