#pragma once

#include "ftk/ui/theme.hpp"

#include <string_view>

namespace ftk::ui {

/// What kind of thing an asset file is, for everywhere an editor shows one:
/// its icon, its hue and the word for it. A program gives one hue to each
/// kind of thing it tells apart (a mesh, its material and its texture can
/// share one); the icon tells those apart.
struct AssetKind {
    const char* noun;  ///< "material", "mesh"
    const char* icon;  ///< from ui::icon
    theme::Hue hue;
};

/// The kind of the file with this extension (".png", ".mesh"). The kit knows
/// the file types any program has: images, fonts and audio clips. Anything
/// else is what a program registered, or a plain "file" in grey.
[[nodiscard]] const AssetKind& asset_kind(std::string_view extension);

/// Shows files with `extension` (".mesh") as `kind` wherever a kind is
/// shown, in place of what was there. A program registers its own file types
/// once, before it draws. `kind`'s noun and icon are kept as given, so they
/// must outlive the program: a string literal and a ui::icon.
void register_asset_kind(std::string_view extension, const AssetKind& kind);

} // namespace ftk::ui
