#pragma once

#include "ftk/ui/theme.hpp"

#include <string_view>

namespace ftk::ui {

/// What kind of thing an asset file is, for everywhere the editor shows one:
/// its icon, its category colour and the word for it. The category is the
/// domain (a mesh, its material and its texture are all Rendering); the icon
/// tells them apart.
struct AssetKind {
    const char* noun;  ///< "material", "mesh"
    const char* icon;  ///< from ui::icon
    theme::Category category;
};

/// The kind of the file with this extension (".png", ".fjmat"). The kit knows
/// the file types any program has: images, fonts and audio clips. Anything
/// else is what a program registered, or a plain "file" in Structure.
[[nodiscard]] const AssetKind& asset_kind(std::string_view extension);

/// Shows files with `extension` (".fjmat") as `kind` wherever a kind is
/// shown, in place of what was there. A program registers its own file types
/// once, before it draws. `kind`'s noun and icon are kept as given, so they
/// must outlive the program: a string literal and a ui::icon.
void register_asset_kind(std::string_view extension, const AssetKind& kind);

} // namespace ftk::ui
