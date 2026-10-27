#pragma once

// Docking: the panels of a dockspace laid out as a tree of splits with
// tabbed leaves, which the user rearranges by dragging tabs (decision 19).
//
// A tree is built once, when a dockspace is first shown, and read back to
// be saved as a named layout or copied to another dockspace:
//
//     using N = ui::DockNode;
//     const N layout = N::left_right(0.2f, N::tabs({"Hierarchy"}), N::tabs({"Viewport", "Console"}));
//     ui::build_dock_tree(dock_id, ui::work_area().size, layout);
//
//     if (auto tree = ui::dock_tree(dock_id, window_names)) save(*tree);
//
// A named layout is the trees of a program's dockspaces, saved as JSON:
//
//     write(path, ui::dock_layout_json({{"scene", *tree}}));
//     if (auto layout = ui::dock_layout_from_json(read(path))) ui::build_dock_tree(dock_id, size, layout->at("scene"));

#include "ftk/ui/types.hpp"

#include <glm/vec2.hpp>

#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ftk::ui {

enum class DockAxis : uint8_t {
    /// A leaf: its windows are tabs.
    None,
    /// Split into a left and a right part.
    LeftRight,
    /// Split into a top and a bottom part.
    TopBottom,
};

/// A node of a dock tree: a split into two, or a leaf of tabbed windows.
struct DockNode {
    DockAxis split{DockAxis::None};
    /// The first part's share of a split: the left or the top.
    float ratio{0.5f};
    /// A split's two parts, the left or top one first.
    std::vector<DockNode> children{};
    /// The windows docked here, by name, in tab order.
    std::vector<std::string> windows{};

    static DockNode left_right(float ratio, DockNode left, DockNode right);
    static DockNode top_bottom(float ratio, DockNode top, DockNode bottom);
    static DockNode tabs(std::initializer_list<std::string> windows);

    bool operator==(const DockNode&) const = default;
};

/// A named layout: the dock trees of a program's dockspaces, each by the
/// name the program gives it, such as an editor's kind of document.
using DockLayout = std::map<std::string, DockNode>;

/// `layout` as JSON: each tree a node with its split ("x" for left and
/// right, "y" for top and bottom), its ratio and its two children, or its
/// windows in tab order.
[[nodiscard]] std::string dock_layout_json(const DockLayout& layout);

/// The layout `json` holds, its nodes' other keys passed over; none when
/// it isn't one.
[[nodiscard]] std::optional<DockLayout> dock_layout_from_json(std::string_view json);

/// The dock trees in a settings file ImGui wrote, `ini`: one for each
/// dockspace its [Docking][Data] holds, each split as ImGui sized it and its
/// windows named as their [Window] entries are, in tab order. A layout saved
/// while a program drew its kit with ImGui converts through it.
[[nodiscard]] std::vector<DockNode> dock_trees_from_imgui_ini(std::string_view ini);

struct DockspaceOptions {
    /// The middle shows what is under the dockspace while nothing is docked
    /// there, such as a scene drawn behind the panels.
    bool see_through{false};
    /// Kept with its layout but not drawn: a dockspace not shown this frame.
    bool hidden{false};
};

/// The dockspace `id`, filling what is left of the window being drawn.
void dockspace(Id id, DockspaceOptions options = {});

/// Whether the keys pressed are for what the dockspace `id` holds: the
/// window that has the keyboard is docked in it, or
/// no window holds the keyboard (none was clicked, the one that was is no
/// longer drawn, or it is a bar that takes no keys of its own). What the
/// owner of a dockspace asks before its shortcuts act on what it edits, so
/// a key pressed in a window outside it is that window's alone.
[[nodiscard]] bool keyboard_in_dockspace(Id id);

/// Whether the window that has the keyboard is docked in the dockspace `id`:
/// of several dockspaces drawn, the one whose owner the keys are for, where
/// keyboard_in_dockspace() also says yes while no window has it.
[[nodiscard]] bool keyboard_docked_in(Id id);

/// Whether the dockspace or dock node `id` exists.
[[nodiscard]] bool dock_exists(Id id);

/// While it lives, the dockspace `id` is built anew, `size` big: what it
/// held is gone, and split_dock(), dock_window() and place_dock_tree() lay
/// it out.
class DockBuild {
public:
    DockBuild(Id id, glm::vec2 size);
    ~DockBuild();
    DockBuild(const DockBuild&) = delete;
    DockBuild& operator=(const DockBuild&) = delete;
    DockBuild(DockBuild&&) = delete;
    DockBuild& operator=(DockBuild&&) = delete;

private:
    Id id_;
};

enum class DockSide : uint8_t { Left, Right, Top, Bottom };

/// The two nodes a split makes.
struct DockSplit {
    /// The part at `side`.
    Id side;
    /// The rest, which keeps the split node's place: what was docked in it,
    /// and the middle of the dockspace if it was.
    Id rest;
};

/// Splits `node` in a DockBuild, giving the part at `side` `ratio` of it.
DockSplit split_dock(Id node, DockSide side, float ratio);

/// Where a window let go on a dock target goes: among the tabs of the leaf
/// `node`, or, with a `side`, in a part of its own at that side of `node` (a
/// leaf, or a dockspace's own id for the whole of it), `share` of it.
struct DockPlace {
    Id node{0};
    std::optional<DockSide> side{};
    float share{0.5f};

    bool operator==(const DockPlace&) const = default;
};

/// Docks the window `name` in `node`, as a tab after those docked there:
/// in a DockBuild, or into a dockspace as the user left it.
void dock_window(const std::string& name, Id node);

/// Docks the window `name` in a part of its own at `side` of the dock node
/// `node`, `share` of it, as a window dropped on that side's target does:
/// into a dockspace as the user left it, the rest staying where it is. A
/// dockspace's own id as `node` puts it beside the whole of it.
/// @return whether it docked: not for a node that doesn't exist, nor for a
///         window docked already, which stays where it is
bool dock_window_beside(const std::string& name, Id node, DockSide side, float share);

/// Docks the window `name` at `place`, as a window carried there and let go
/// does: out of the part it was docked in first, which folds away when it
/// is left empty.
/// @return whether it docked: not when `place`'s node is gone, nor among the
///         tabs of a node that is split, the window left where it was
bool dock_window_at(const std::string& name, const DockPlace& place);

/// Lays `tree` out in `node`, in a DockBuild: its splits, then its windows.
/// A split's second part keeps `node`'s place (split_dock()).
void place_dock_tree(Id node, const DockNode& tree);

/// The dockspace `id` built anew as `tree`, `size` big.
void build_dock_tree(Id id, glm::vec2 size, const DockNode& tree);

/// The tree of the dockspace `id` as the user left it, holding those of
/// `windows` docked in it; none when there is no such dockspace. A window
/// not shown this run is found where the settings file last had it.
[[nodiscard]] std::optional<DockNode> dock_tree(Id id, std::span<const std::string> windows);

/// The dock node the window `name` is in, or 0 when it floats or does not
/// exist yet.
[[nodiscard]] Id window_dock(const std::string& name);

/// The dockspace the dock node `node` is part of: its own id for a
/// dockspace; 0 for no node.
[[nodiscard]] Id dockspace_of(Id node);

} // namespace ftk::ui
