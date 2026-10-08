#pragma once

#include "ftk/ui/types.hpp"

#include <string_view>

// One of a list picked by its picture (sheet 45, B1), as a project's
// template is: a small picture at the left, the name, and a line about it
// under the name, both cut with an ellipsis to the row's width. The picked
// row is washed in the selection colour, the one under the mouse lit. Like
// a tree row, it is the last item after it is drawn, for the owner's menu.
//
//     if (ui::picture_row({.id = "empty", .picture = picture, .name = "Empty", .detail = "Lit ground"},
//                         picked == 0).clicked) {
//         picked = 0;
//     }

namespace ftk::ui {

struct PictureRowSpec {
    /// Unique among the list's rows; also the row's id.
    const char* id{""};
    /// The row's picture, 16:9. Without one, `icon` stands in its place on a
    /// sunken frame.
    Texture picture{};
    const char* icon{nullptr};
    std::string_view name{};
    std::string_view detail{};
};

struct PictureRowResult {
    bool clicked{false};
    bool double_clicked{false};
};

/// A row as wide as the space left.
PictureRowResult picture_row(const PictureRowSpec& spec, bool selected);

} // namespace ftk::ui
