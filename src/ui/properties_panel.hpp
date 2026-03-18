#pragma once

#include "scene/transform.hpp"

#include <cstdint>
#include <string>

namespace fjell {

class CommandHistory;
class Scene;
class ScriptManager;
struct DirectionalLight;

class PropertiesPanel {
public:
    void init(Scene* scene, uint32_t* selected_id, int* gizmo_op,
              DirectionalLight* light, ScriptManager* scripts,
              CommandHistory* history);

    void draw();

private:
    Scene* scene_{nullptr};
    uint32_t* selected_id_{nullptr};
    int* gizmo_op_{nullptr};
    DirectionalLight* light_{nullptr};
    ScriptManager* scripts_{nullptr};
    CommandHistory* history_{nullptr};

    // Drag coalescing
    Transform drag_start_transform_;
    std::string drag_start_name_;
};

} // namespace fjell
