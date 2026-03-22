// fjell_component.hpp — ScriptComponent base class for Fjell game code
// Shipped to game projects alongside fjell_api.h.

#pragma once

#include "fjell_transform.hpp"
#include "fjell_world.hpp"

#include <cstdint>

namespace fjell {

class ScriptManager;
struct SceneNode;

class ScriptComponent {
public:
    virtual ~ScriptComponent() = default;

    virtual void begin_play() {}
    virtual void tick(float dt) { (void)dt; }
    virtual void end_play() {}

    [[nodiscard]] uint32_t id() const { return id_; }
    [[nodiscard]] const char* name() const { return name_; }
    [[nodiscard]] Transform& transform() { return *transform_; }
    [[nodiscard]] const Transform& transform() const { return *transform_; }
    [[nodiscard]] World* world() { return world_; }
    [[nodiscard]] const World* world() const { return world_; }
    [[nodiscard]] SceneNode* node() { return node_; }
    [[nodiscard]] const SceneNode* node() const { return node_; }

private:
    friend class ScriptManager;
    SceneNode* node_{nullptr};
    World* world_{nullptr};
    Transform* transform_{nullptr};
    uint32_t id_{0};
    const char* name_{""};
};

// Short alias for game scripts
using Component = ScriptComponent;

} // namespace fjell
