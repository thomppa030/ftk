#pragma once

#include <cstdint>
#include <functional>
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
    using AddPrimitiveCallback = std::function<void(const std::string&)>;
    using RemoveObjectCallback = std::function<void(uint32_t)>;

    void init(Scene* scene, ViewportManager* viewports, uint32_t* selected_id,
              CommandHistory* history, IconCache* icons = nullptr);
    void set_add_primitive_callback(AddPrimitiveCallback cb) { add_primitive_cb_ = std::move(cb); }
    void set_remove_object_callback(RemoveObjectCallback cb) { remove_object_cb_ = std::move(cb); }

    void draw();

private:
    void draw_node(SceneNode& obj);

    Scene* scene_{nullptr};
    ViewportManager* viewports_{nullptr};
    uint32_t* selected_id_{nullptr};
    CommandHistory* history_{nullptr};

    IconCache* icons_{nullptr};
    uint32_t remove_pending_id_{0};

    AddPrimitiveCallback add_primitive_cb_;
    RemoveObjectCallback remove_object_cb_;

    // Per-frame parent→children lookup (avoids O(n²) scans)
    std::unordered_map<int32_t, std::vector<SceneNode*>> children_map_;
};

} // namespace fjell
