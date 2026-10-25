#pragma once

#include "ftk/ui/kit/rows_in_view.hpp"
#include "ftk/ui/theme.hpp"
#include "ftk/ui/types.hpp"

#include <glm/vec2.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// Trees: the hierarchy, the bone tree, the UI element tree. Each row is 24
// px with a chevron when it has children, a guide line per level, the
// node's icon tinted in its hue, its name, and a dot per hue of
// what is composed onto it. The owner walks its own tree and decides what
// is selected; the row draws and reports. A drag resting on a folded row
// for 0.6 s opens it, so a row inside it can be reached.
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

    /// Finishes it as Enter does, for an owner moving on before the field
    /// is drawn again (another name started in the same box): the name as
    /// typed when it differs from the old and isn't empty.
    std::optional<std::string> finish();

private:
    std::optional<std::uint32_t> key_;
    std::string original_;
    std::string text_;
    bool focus_{false};
    /// The field's id as last drawn, which finish() takes the keyboard from.
    Id field_{0};
};

struct TreeRowSpec {
    /// Unique among the tree's rows; also the row's id.
    const char* id{""};
    std::string_view name{};
    int depth{0};
    bool has_children{false};
    /// The node's icon from ui::icon, and the hue it is tinted in; a
    /// plain node (an empty, a mesh) is in the secondary text colour.
    const char* icon{nullptr};
    std::optional<theme::Hue> hue{};
    /// Colours the icon in place of its hue (a preset).
    std::optional<Color> tint{};
    /// One dot per hue composed onto the node, in this order.
    std::span<const theme::Hue> dots{};
    /// A quiet word at the row's end, before the dots, small and in the
    /// disabled text colour: what kind of row it is ("persistent" on a
    /// scene's). A long name stops short of it.
    std::string_view tag{};
    /// What the row stands for, after the name in the body face and the
    /// secondary colour (the text colour on the row shown): a `:`
    /// command's words beside its name. It begins `description_at` in from
    /// where the name begins, so the rows' descriptions stand in a column,
    /// or after a longer name; it is cut short of the tag.
    std::string_view description{};
    float description_at{0.0f};
    RowSelection selection{RowSelection::None};
    /// The search being shown: the match in the name is amber. A row kept
    /// only because something under it matches is `dimmed`.
    std::string_view query{};
    bool dimmed{false};
    /// Open whatever the user folded, while a search shows what is inside.
    bool force_open{false};
    /// Whether the row is open before the user first folds or unfolds it.
    bool starts_open{true};
    /// A press anywhere on the row folds or unfolds it, as on a file tree's
    /// folder, and still counts as a click; otherwise only the chevron
    /// folds.
    bool fold_on_click{false};
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
    float outer_gap_{0.0f};
};

TreeRowResult tree_row(const TreeRowSpec& spec);

/// What an open row holds that is no row of the tree (what a save keeps of a
/// node, sheet 54), for as long as it lives: a sunken box under the row,
/// across the tree, what it holds inside its padding at the spacing the window
/// has around the tree and on the inset field colours, as an inset group's
/// is. The rows after it go on under it.
///
///     const auto row = ui::tree_row({.id = "7", .name = "Net Rack", .has_children = true});
///     if (row.open) {
///         if (auto inset = ui::TreeInset("##kept")) draw_kept(node);
///     }
class TreeInset {
public:
    explicit TreeInset(const char* id);
    ~TreeInset();
    TreeInset(const TreeInset&) = delete;
    TreeInset& operator=(const TreeInset&) = delete;
    TreeInset(TreeInset&&) = delete;
    TreeInset& operator=(TreeInset&&) = delete;
    explicit operator bool() const { return true; }

private:
    Id height_id_{0};
    glm::vec2 min_{};
    float width_{0.0f};
};

/// Whether row `id` of the tree being drawn is open, as tree_row() would
/// draw it: for an owner listing the rows its tree shows before drawing the
/// ones in sight (RowsInView).
[[nodiscard]] bool tree_row_open(const char* id, bool starts_open = true);

/// Opens row `id` of the tree being drawn, as unfolding it from its chevron
/// would: for an owner bringing a row under closed folds into view.
void open_tree_row(const char* id);

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
void drag_preview(const char* icon, std::optional<theme::Hue> hue, std::string_view text,
                  std::string_view refusal = {});

} // namespace ftk::ui
