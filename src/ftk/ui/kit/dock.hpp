#pragma once

// Docking: the panels of a dockspace laid out as a tree of splits with
// tabbed leaves, which the user rearranges by dragging tabs (decision 19).
// While ImGui draws the kit, the tree is ImGui's dock nodes.
//
// A tree is built once, when a dockspace is first shown, and read back to
// be saved as a named layout or copied to another dockspace:
//
//     using N = ui::DockNode;
//     const N layout = N::left_right(0.2f, N::tabs({"Hierarchy"}), N::tabs({"Viewport", "Console"}));
//     ui::build_dock_tree(dock_id, ui::work_area().size, layout);
//
//     if (auto tree = ui::dock_tree(dock_id, window_names)) save(*tree);

#include "ftk/ui/types.hpp"

#include <glm/vec2.hpp>

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
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
};

struct DockspaceOptions {
    /// The middle shows what is under the dockspace while nothing is docked
    /// there, such as a scene drawn behind the panels.
    bool see_through{false};
    /// Kept with its layout but not drawn: a dockspace not shown this frame.
    bool hidden{false};
};

/// The dockspace `id`, filling what is left of the window being drawn.
void dockspace(Id id, DockspaceOptions options = {});

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

/// Docks the window `name` in `node`, as a tab after those docked there:
/// in a DockBuild, or into a dockspace as the user left it.
void dock_window(const std::string& name, Id node);

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

} // namespace ftk::ui
