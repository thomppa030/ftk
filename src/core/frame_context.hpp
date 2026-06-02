#pragma once

namespace fjell {

class Clock;
class Input;
class PhysicsWorld;
class Project;
class ResourceManager;
class Scene;

struct TickContext {
    Scene&         scene;
    const Input&   input;
    const Clock&   clock;
    float          dt;

    PhysicsWorld*    physics{nullptr};
    const Project*   project{nullptr};
    ResourceManager* resources{nullptr};
};

} // namespace fjell
