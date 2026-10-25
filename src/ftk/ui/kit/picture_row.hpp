#pragma once

#include "ftk/ui/types.hpp"

#include <cstdint>
#include <string_view>

// One of a list picked by its picture: the picture at the left, the name and
// the lines about it beside it, each cut with an ellipsis to the row's width.
// Small (sheet 45, B1), as a project's template is: a 64 × 36 picture, the
// name and a line about it. Large (sheet 55, A1), as a project in the hub is:
// a 128 × 72 picture, the name, its folder in the mono face and a line about
// it. The picked row is washed in the selection colour, the one under the
// mouse lit. A row whose thing can't be used now is dimmed, its picture in
// greys, and says why on its last line in the error colour. Like a tree row,
// it is the last item after it is drawn, for the owner's menu.
//
//     if (ui::picture_row({.id = "empty", .picture = picture, .name = "Empty", .detail = "Lit ground"},
//                         picked == 0).clicked) {
//         picked = 0;
//     }
//
//     ui::picture_row({.id = "fjara", .picture = shot, .icon = icon::folder, .name = "fjara",
//                      .detail = "Opened 2 hours ago", .size = ui::PictureRowSize::large,
//                      .path = "~/fjara", .query = search},
//                     false);

namespace ftk::ui {

enum class PictureRowSize : uint8_t {
    /// A 64 × 36 picture.
    small,
    /// A 128 × 72 picture.
    large,
};

struct PictureRowSpec {
    /// Unique among the list's rows; also the row's id.
    const char* id{""};
    /// The row's picture, 16:9. Without one, `icon` stands in its place on a
    /// sunken frame.
    Texture picture{};
    const char* icon{nullptr};
    std::string_view name{};
    /// The line about it, last, in the secondary colour.
    std::string_view detail{};
    PictureRowSize size{PictureRowSize::small};
    /// The file or folder the row stands for, on a line of its own under the
    /// name in the mono face; none when empty.
    std::string_view path{};
    /// Why what the row stands for can't be used, in place of the detail: in
    /// the error colour behind its icon.
    std::string_view problem{};
    /// What the row stands for can't be used now: its picture in greys and
    /// faded, its name and path dimmed. It is still hovered and clicked, for
    /// its tooltip and its menu.
    bool unavailable{false};
    /// Said when the row is hovered.
    std::string_view tooltip{};
    /// The search being shown: its match in the name and the path is amber.
    std::string_view query{};
};

struct PictureRowResult {
    bool clicked{false};
    bool double_clicked{false};
};

/// A row as wide as the space left.
PictureRowResult picture_row(const PictureRowSpec& spec, bool selected);

} // namespace ftk::ui
