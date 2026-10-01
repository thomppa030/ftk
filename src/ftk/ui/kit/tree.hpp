#pragma once

#include "ftk/ui/theme.hpp"

#include <imgui.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// Trees: the hierarchy, the bone tree, the UI element tree. Each row is 24
// px with a chevron when it has children, a guide line per level, the
// node's icon tinted by its category, its name, and a dot per category of
// what is composed onto it. The owner walks its own tree and decides what
// is selected; the row draws and reports.
//
//     if (auto tree = ui::Tree("##scene")) {
//         const auto row = ui::tree_row({.id = "12", .name = node.name, .depth = 1, ...});
//         if (row.clicked) select(node);
//         if (row.open) { ...children... }
//     }
//
// After tree_row() the row is the last item, so a context menu, drag source
// or tooltip attaches to it the usual way.

namespace ftk::ui {

/// How a row is selected: not at all, as the one being edited, or along with
/// it in a multi-selection.
enum class RowSelection { None, Primary, Secondary };

/// A name being edited in its row: a sunken field edged in amber, the text
/// where the name was. The owner starts it (F2, the context menu, a new
/// node) and keeps it; the row whose key it holds draws the field in place
/// of its name.
class RenameBox {
public:
    /// Opens the field on `key`'s row with `name` in it, selected.
    void start(std::uint32_t key, std::string_view name);
    void cancel() { key_.reset(); }
    [[nodiscard]] bool editing(std::uint32_t key) const { return key_ == key; }
    [[nodiscard]] bool editing() const { return key_.has_value(); }

    /// Draws the field in `width` at the cursor. Enter or a click elsewhere
    /// finishes it, and returns the new name when it differs from the old
    /// and isn't empty; Esc leaves the old one.
    std::optional<std::string> draw(float width);

private:
    std::optional<std::uint32_t> key_;
    std::string original_;
    std::string text_;
    bool focus_{false};
};

struct TreeRowSpec {
    /// Unique among the tree's rows; also the ImGui ID.
    const char* id{""};
    std::string_view name{};
    int depth{0};
    bool has_children{false};
    /// The node's icon from ui::icon, and the category it is tinted in; a
    /// plain node (an empty, a mesh) is in the secondary text colour.
    const char* icon{nullptr};
    std::optional<theme::Category> category{};
    /// Colours the icon in place of the category's (a preset).
    std::optional<ImVec4> tint{};
    /// One dot per category composed onto the node, in this order.
    std::span<const theme::Category> dots{};
    RowSelection selection{RowSelection::None};
    /// The search being shown: the match in the name is amber. A row kept
    /// only because something under it matches is `dimmed`.
    std::string_view query{};
    bool dimmed{false};
    /// Open whatever the user folded, while a search shows what is inside.
    bool force_open{false};
    /// Whether the row is open before the user first folds or unfolds it.
    bool starts_open{true};
    /// The owner's rename, drawn in this row when it holds `key`.
    RenameBox* rename{nullptr};
    std::uint32_t key{0};
};

struct TreeRowResult {
    /// The row's children are to be drawn.
    bool open{false};
    /// Pressed on the row (the chevron only folds).
    bool clicked{false};
    bool double_clicked{false};
    /// The rename in this row finished with a new name.
    std::optional<std::string> renamed{};
};

/// The scope rows are drawn in: they sit edge to edge. A `text_size` other
/// than 0 draws the rows' text at that size, and their height, indent and
/// chevron scale with it so a larger size keeps its spacing.
class Tree {
public:
    explicit Tree(const char* id, float text_size = 0.0f);
    ~Tree();
    Tree(const Tree&) = delete;
    Tree& operator=(const Tree&) = delete;
    Tree(Tree&&) = delete;
    Tree& operator=(Tree&&) = delete;
    explicit operator bool() const { return true; }

private:
    bool sized_{false};
    float outer_size_{0.0f};
};

TreeRowResult tree_row(const TreeRowSpec& spec);

/// Where, over the last row drawn, a dragged row would land: in front of it
/// (its top quarter), after it (its bottom quarter), or into it.
enum class DropPlace { Before, Into, After };
[[nodiscard]] DropPlace drop_place();

/// Shows where a drop over the last row lands: an amber insert line at its
/// top or bottom, with a ring whose indent is `depth`, the depth the dropped
/// rows will have; or, into the row, the row outlined. A drop that can't
/// happen draws nothing (drag_preview says why).
void draw_drop(DropPlace place, int depth);

/// What is being dragged, under the cursor: the row's icon and name, or a
/// count for several. `refusal` says why the target can't take them.
void drag_preview(const char* icon, std::optional<theme::Category> category, std::string_view text,
                  std::string_view refusal = {});

} // namespace ftk::ui
