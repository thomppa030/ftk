#pragma once

#include "core/delegate.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fjell {

class CommandHistory;
class IconCache;
class Scene;
class ViewportManager;
struct Node;

class HierarchyPanel {
public:
    void init(Scene* scene, ViewportManager* viewports, uint32_t* selected_id,
              CommandHistory* history, IconCache* icons = nullptr);

    void draw();

    // Multi-selection API
    [[nodiscard]] const std::unordered_set<uint32_t>& selection() const { return selection_; }
    void select(uint32_t id);          // set as sole selection
    void toggle_select(uint32_t id);   // Ctrl+click
    void range_select(uint32_t id);    // Shift+click
    void add_to_selection(uint32_t id);
    void clear_selection();

private:
    void draw_node(Node& obj);

    Scene* scene_{nullptr};
    ViewportManager* viewports_{nullptr};
    uint32_t* selected_id_{nullptr};
    CommandHistory* history_{nullptr};

    IconCache* icons_{nullptr};
    uint32_t remove_pending_id_{0};

    // Search/filter state
    char filter_text_[128]{};
    int type_filter_{0}; // 0=All, 1=Mesh, 2=Light, 3=Camera, 4=Folder, 5=Empty

    [[nodiscard]] bool matches_filter(const Node& obj) const;
    [[nodiscard]] bool subtree_matches_filter(const Node& obj) const;
    [[nodiscard]] bool is_filtering() const { return filter_text_[0] != '\0' || type_filter_ != 0; }

    // Multi-selection state
    std::unordered_set<uint32_t> selection_;
    uint32_t last_clicked_id_{0}; // anchor for shift-click range select

public:
    Delegate<void(const std::string&)> on_add_primitive;
    Delegate<void(uint32_t)> on_remove_object;
    Delegate<void()> on_duplicate;

private:

    // Per-frame parent→children lookup (avoids O(n²) scans)
    std::unordered_map<int32_t, std::vector<Node*>> children_map_;

    // Flat draw-order list for shift-click range selection
    std::vector<uint32_t> draw_order_;
};

} // namespace fjell
