#pragma once

#include "ui/theme.hpp"

#include <string_view>

namespace fjell::ui {

/// What kind of thing an asset file is, for everywhere the editor shows one:
/// its icon, its category colour and the word for it. The category is the
/// domain (a mesh, its material and its texture are all Rendering); the icon
/// tells them apart.
struct AssetKind {
    const char* noun;  ///< "material", "mesh"
    const char* icon;  ///< from ui::icon
    theme::Category category;
};

/// The kind of the file with this extension (".fjmat", ".png"). Unknown
/// extensions are a plain "file" in Structure.
[[nodiscard]] const AssetKind& asset_kind(std::string_view extension);

} // namespace fjell::ui
