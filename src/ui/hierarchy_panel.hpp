#pragma once

#include "ftk/base/delegate.hpp"
#include "ftk/ui/kit/tree.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct ImGuiPayload;

namespace fjell {

class CommandHistory;
class Scene;
class ViewportManager;
struct Node;

class HierarchyPanel {
public:
    void init(Scene* scene, ViewportManager* viewports, uint32_t* selected_id,
              CommandHistory* history);

    void draw();

    // Multi-selection API
    [[nodiscard]] const std::unordered_set<uint32_t>& selection() const { return selection_; }
    void select(uint32_t id);          // set as sole selection
    void toggle_select(uint32_t id);   // Ctrl+click
    void range_select(uint32_t id);    // Shift+click
    void add_to_selection(uint32_t id);
    void clear_selection();

private:
    /// The settings button's menu: the tree's text size.
    void draw_settings_menu();
    void draw_node(Node& obj, int depth);
    // Adds a node of `type` from the add menu (empty when nothing was
    // picked); a new empty opens in rename.
    void add_object(const std::string& type);
    // Frames the viewport's camera on the node, as a double-click does.
    void frame(const Node& obj);

    // Drag and drop. A drop lands under `parent_id` (-1 for the top), in
    // front of `before_id` (0 for last), at `depth` in the tree.
    struct Target {
        int32_t parent_id;
        uint32_t before_id;
        int depth;
    };
    // What dragging the row drags: the selection when the row is in it.
    [[nodiscard]] std::vector<uint32_t> dragged_ids(uint32_t row_id) const;
    [[nodiscard]] static std::vector<uint32_t> payload_ids(const ImGuiPayload& payload);
    [[nodiscard]] Target drop_target(const Node& row, int depth, bool open, ui::DropPlace place,
                                     const std::vector<uint32_t>& ids) const;
    // Why the nodes can't go under `parent_id`, or empty when they can.
    [[nodiscard]] std::string refuse_drop(const std::vector<uint32_t>& ids, int32_t parent_id) const;
    void move_nodes(const std::vector<uint32_t>& ids, int32_t parent_id, uint32_t before_id);
    // Why the drop under the cursor is refused: found this frame, shown by
    // the drag preview the next.
    std::string refusal_now_;
    std::string refusal_shown_;

    // Inline rename of one row, started by F2 on the primary selection or
    // from the row's context menu.
    ui::RenameBox rename_;

    Scene* scene_{nullptr};
    ViewportManager* viewports_{nullptr};
    uint32_t* selected_id_{nullptr};
    CommandHistory* history_{nullptr};

    // Delete from a row's context menu, carried out after the tree is drawn.
    bool delete_pending_{false};

    // Search/filter state
    std::string filter_text_;
    int type_filter_{0}; // 0=All, 1=Mesh, 2=Light, 3=Camera, 4=Folder, 5=Empty

    [[nodiscard]] bool matches_filter(const Node& obj) const;
    [[nodiscard]] bool subtree_matches_filter(const Node& obj) const;
    [[nodiscard]] bool is_filtering() const { return !filter_text_.empty() || type_filter_ != 0; }

    // Multi-selection state
    std::unordered_set<uint32_t> selection_;
    uint32_t last_clicked_id_{0}; // anchor for shift-click range select

public:
    Delegate<void(const std::string&)> on_add_primitive;
    // Delete everything selected, as one undo step.
    Delegate<void()> on_delete_selection;
    Delegate<void()> on_duplicate;
    // Preset: instantiate a .fjp (path, parent_id to instantiate under)
    Delegate<void(const std::string&, int32_t)> on_instantiate_preset;

private:

    // Per-frame parent→children lookup (avoids O(n²) scans)
    std::unordered_map<int32_t, std::vector<Node*>> children_map_;

    // Flat draw-order list for shift-click range selection
    std::vector<uint32_t> draw_order_;
};

} // namespace fjell
