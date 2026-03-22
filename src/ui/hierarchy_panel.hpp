#pragma once

#include "core/delegate.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace fjell {

class CommandHistory;
class IconCache;
class Scene;
class ViewportManager;
struct SceneNode;

class HierarchyPanel {
public:
    void init(Scene* scene, ViewportManager* viewports, uint32_t* selected_id,
              CommandHistory* history, IconCache* icons = nullptr);

    void draw();

private:
    void draw_node(SceneNode& obj);

    Scene* scene_{nullptr};
    ViewportManager* viewports_{nullptr};
    uint32_t* selected_id_{nullptr};
    CommandHistory* history_{nullptr};

    IconCache* icons_{nullptr};
    uint32_t remove_pending_id_{0};

public:
    Delegate<void(const std::string&)> on_add_primitive;
    Delegate<void(uint32_t)> on_remove_object;

private:

    // Per-frame parent→children lookup (avoids O(n²) scans)
    std::unordered_map<int32_t, std::vector<SceneNode*>> children_map_;
};

} // namespace fjell
