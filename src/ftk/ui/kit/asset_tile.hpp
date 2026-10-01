#pragma once

#include "ftk/ui/kit/tree.hpp"
#include "ftk/ui/theme.hpp"

#include <imgui.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// A file in the content browser's grid: 96 px wide, its picture over its
// name. The picture is the asset's thumbnail when it has one, else the icon
// of its kind large in the kind's colour. The name takes two lines, cut with
// an ellipsis, and is whole in the tooltip. Like a tree row, the tile is the
// last item after it is drawn, for the owner's menus and drags.

namespace ftk::ui {

struct AssetTileSpec {
    /// Unique among the grid's tiles; also the ImGui ID.
    const char* id{""};
    std::string_view name{};
    /// The tile's picture: a rendered thumbnail, or an icon image; 0 for
    /// none. Drawn as it is unless `tint` is set.
    ImTextureID picture{0};
    /// Without a picture: the kind's icon glyph and colour. `tint` colours
    /// the picture or overrides its hue (a folder the user
    /// coloured).
    const char* icon{nullptr};
    std::optional<theme::Hue> hue{};
    std::optional<ImVec4> tint{};
    bool selected{false};
    /// The search being shown: its match in the name is amber.
    std::string_view query{};
    /// The owner's rename, drawn in this tile when it holds `key`.
    RenameBox* rename{nullptr};
    std::uint32_t key{0};
};

struct AssetTileResult {
    bool clicked{false};
    bool double_clicked{false};
    /// The rename in this tile finished with a new name.
    std::optional<std::string> renamed{};
};

AssetTileResult asset_tile(const AssetTileSpec& spec);

/// The size one tile takes, for laying out a grid of them.
[[nodiscard]] ImVec2 asset_tile_size();

} // namespace ftk::ui
