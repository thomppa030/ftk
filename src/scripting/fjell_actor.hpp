// fjell_actor.hpp — Actor base class for Fjell game code
// Shipped to game projects alongside fjell_api.h.

#pragma once

#include "fjell_transform.hpp"

#include <cstdint>

namespace fjell {

class ScriptComponent;
class ScriptManager;
struct World;

class Actor {
public:
    virtual ~Actor() = default;

    virtual void begin_play() {}
    virtual void tick(float dt) { (void)dt; }
    virtual void end_play() {}

    [[nodiscard]] uint32_t id() const { return id_; }
    [[nodiscard]] const char* name() const { return name_; }
    [[nodiscard]] World* world() { return world_; }
    [[nodiscard]] const World* world() const { return world_; }
    [[nodiscard]] Transform& transform() { return *transform_; }
    [[nodiscard]] const Transform& transform() const { return *transform_; }

    // Engine sets these — game code should not write to them directly
    uint32_t id_{0};
    int32_t parent_id_{-1};
    const char* name_{""};

private:
    friend class ScriptManager;
    World* world_{nullptr};
    Transform* transform_{nullptr};
};

} // namespace fjell
